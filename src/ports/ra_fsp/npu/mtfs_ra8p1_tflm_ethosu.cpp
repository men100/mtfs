#include "mtfs_ra8p1_tflm_ethosu.h"

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "bsp_api.h"
#include "flatbuffers/flatbuffers.h"
#include "mtfs_ra8p1_ethosu_instance.h"
#include "../../../sentinel/mtfs_sentinel_sha256.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace {

constexpr uint32_t kFeatureCount = 24U;
constexpr uint32_t kInterpreterOffset = 0U;
constexpr uint32_t kResolverOffset = 224U;
constexpr uint32_t kArenaOffset = 288U;
constexpr uint32_t kContextOffset = 4384U;
constexpr uint32_t kInterpreterLogical = 196U;
constexpr uint32_t kResolverLogical = 48U;
constexpr uint32_t kAcceleratorContext = 96U;
constexpr uint32_t kSemaphorePool = 16U;
constexpr uint32_t kProviderSync = 4U;
constexpr uint8_t kAllModelBasePointers = 0x1fU;

using Resolver = tflite::MicroMutableOpResolver<1>;

struct Session {
  const tflite::Model* model;
  Resolver* resolver;
  tflite::MicroInterpreter* interpreter;
  TfLiteTensor* input;
  TfLiteTensor* output;
  uint8_t* persistent;
  uint32_t arena_used;
  bool resolver_constructed;
  bool interpreter_constructed;
};

struct GlobalState {
  bool open;
  uint32_t active_sessions;
  uint32_t opens;
  uint32_t open_failures;
};

GlobalState g_global{};

static_assert(sizeof(tflite::MicroInterpreter) == kInterpreterLogical,
              "TFLM interpreter size changed; regenerate the RA descriptor");
static_assert(sizeof(Resolver) == kResolverLogical,
              "TFLM resolver size changed; regenerate the RA descriptor");
static_assert(sizeof(Session) <= MTFS_RA8P1_ETHOSU_CONTEXT_SIZE,
              "RA provider session context exceeds its descriptor region");
static_assert(sizeof(rm_ethosu_instance_ctrl_t) + sizeof(struct ethosu_driver) ==
                  kAcceleratorContext,
              "FSP/Ethos-U global context changed; regenerate the descriptor");

void SetDiagnostic(mtfs_ra8p1_tflm_ethosu_t* target, uint32_t stage,
                   int32_t detail, uint32_t expected, uint32_t actual) {
  target->diagnostics.diagnostic_stage = stage;
  target->diagnostics.diagnostic_detail = detail;
  target->diagnostics.diagnostic_expected = expected;
  target->diagnostics.diagnostic_actual = actual;
}

bool IsFeatureShape(const flatbuffers::Vector<int32_t>* shape) {
  return shape != nullptr && shape->size() == 2U && shape->Get(0U) == 1 &&
         shape->Get(1U) == static_cast<int32_t>(kFeatureCount);
}

bool QuantizationMatches(const tflite::Tensor* tensor, uint32_t numerator,
                         uint32_t shift, int8_t zero_point) {
  if (tensor == nullptr || tensor->type() != tflite::TensorType_INT8 ||
      !IsFeatureShape(tensor->shape()) || shift > 31U || numerator == 0U) {
    return false;
  }
  const tflite::QuantizationParameters* quant = tensor->quantization();
  if (quant == nullptr || quant->scale() == nullptr ||
      quant->zero_point() == nullptr || quant->scale()->size() != 1U ||
      quant->zero_point()->size() != 1U ||
      quant->zero_point()->Get(0U) != zero_point) {
    return false;
  }
  const float expected = static_cast<float>(numerator) /
                         static_cast<float>(UINT32_C(1) << shift);
  uint32_t expected_bits = 0U;
  uint32_t actual_bits = 0U;
  std::memcpy(&expected_bits, &expected, sizeof(expected_bits));
  const float actual = quant->scale()->Get(0U);
  std::memcpy(&actual_bits, &actual, sizeof(actual_bits));
  return expected_bits == actual_bits;
}

const tflite::Tensor* FindTensor(const tflite::SubGraph* graph,
                                const char* name) {
  if (graph == nullptr || graph->tensors() == nullptr) return nullptr;
  for (uint32_t i = 0U; i < graph->tensors()->size(); ++i) {
    const tflite::Tensor* tensor = graph->tensors()->Get(i);
    if (tensor != nullptr && tensor->name() != nullptr &&
        std::strcmp(tensor->name()->c_str(), name) == 0) {
      return tensor;
    }
  }
  return nullptr;
}

bool BufferSizeIs(const tflite::Model* model, const tflite::Tensor* tensor,
                  uint32_t expected) {
  if (model == nullptr || tensor == nullptr || model->buffers() == nullptr ||
      tensor->buffer() >= model->buffers()->size()) return false;
  const tflite::Buffer* buffer = model->buffers()->Get(tensor->buffer());
  const uint32_t actual = buffer == nullptr || buffer->data() == nullptr ? 0U :
                          buffer->data()->size();
  return actual == expected;
}

bool ValidateModel(const uint8_t* binary, uint32_t binary_size,
                   const tflite::Model** result) {
  flatbuffers::Verifier verifier(binary, binary_size);
  if (!tflite::VerifyModelBuffer(verifier)) return false;
  const tflite::Model* model = tflite::GetModel(binary);
  if (model == nullptr || model->version() != TFLITE_SCHEMA_VERSION ||
      model->subgraphs() == nullptr || model->subgraphs()->size() != 1U ||
      model->operator_codes() == nullptr ||
      model->operator_codes()->size() != 1U) return false;
  const tflite::OperatorCode* code = model->operator_codes()->Get(0U);
  const flatbuffers::String* custom = code == nullptr ? nullptr :
                                      code->custom_code();
  const tflite::SubGraph* graph = model->subgraphs()->Get(0U);
  if (code == nullptr || code->builtin_code() != tflite::BuiltinOperator_CUSTOM ||
      custom == nullptr || custom->size() != 7U ||
      std::memcmp(custom->c_str(), "ethos-u", 7U) != 0 || graph == nullptr ||
      graph->operators() == nullptr || graph->operators()->size() != 1U ||
      graph->inputs() == nullptr || graph->inputs()->size() != 1U ||
      graph->outputs() == nullptr || graph->outputs()->size() != 1U) return false;
  const tflite::Tensor* command = FindTensor(graph, "ethos_u_command_stream");
  const tflite::Tensor* weights = FindTensor(graph, "read_only");
  const tflite::Tensor* scratch = FindTensor(graph, "scratch");
  if (!BufferSizeIs(model, command, 576U) ||
      !BufferSizeIs(model, weights, 1536U) || scratch == nullptr ||
      scratch->shape() == nullptr || scratch->shape()->size() != 1U ||
      scratch->shape()->Get(0U) != 48) return false;
  *result = model;
  return true;
}

void FillRegion(mtfs_sentinel_npu_actual_region_t* region, uint16_t kind,
                uint16_t placement, uint32_t alignment, uint64_t logical,
                uint64_t storage, uint64_t address) {
  region->kind = kind;
  region->placement = placement;
  region->alignment = alignment;
  region->logical_size = logical;
  region->storage_size = storage;
  region->address_or_offset = address;
}

mtfs_error_t InspectRuntime(void* opaque, const uint8_t* binary,
                            uint32_t binary_size,
                            const mtfs_sentinel_runtime_info_t* runtime,
                            mtfs_sentinel_npu_actual_info_t* actual) {
  auto* target = static_cast<mtfs_ra8p1_tflm_ethosu_t*>(opaque);
  const tflite::Model* model = nullptr;
  if (target == nullptr || binary == nullptr || runtime == nullptr ||
      actual == nullptr ||
      (reinterpret_cast<uintptr_t>(binary) & 31U) != 0U) {
    return MTFS_ERROR_INVALID_ARGUMENT;
  }
  SetDiagnostic(target, 1U, 0, 0U, 0U);
  if (!ValidateModel(binary, binary_size, &model)) {
    SetDiagnostic(target, 2U, 0, 1U, 0U);
    return MTFS_ERROR_UNSUPPORTED_FORMAT;
  }
  const tflite::SubGraph* graph = model->subgraphs()->Get(0U);
  const tflite::Tensor* input = graph->tensors()->Get(graph->inputs()->Get(0U));
  const tflite::Tensor* output = graph->tensors()->Get(graph->outputs()->Get(0U));
  if (!QuantizationMatches(input, runtime->input_scale_numerator,
                           runtime->input_scale_shift,
                           runtime->input_zero_point) ||
      !QuantizationMatches(output, runtime->output_scale_numerator,
                           runtime->output_scale_shift,
                           runtime->output_zero_point)) {
    SetDiagnostic(target, 4U, 0, 1U, 0U);
    return MTFS_ERROR_UNSUPPORTED_FORMAT;
  }
  std::memset(actual, 0, sizeof(*actual));
  actual->runtime_abi = MTFS_RA8P1_ETHOSU_RUNTIME_ABI;
  actual->runtime_variant = MTFS_RA8P1_ETHOSU_RUNTIME_VARIANT;
  actual->runtime_extra = MTFS_RA8P1_ETHOSU_RUNTIME_EXTRA;
  actual->copy_alignment = 32U;
  actual->copy_size = MTFS_RA8P1_ETHOSU_PERSISTENT_SIZE;
  FillRegion(&actual->regions[0], MTFS_SENTINEL_REGION_RUNTIME_BINARY,
             MTFS_SENTINEL_PLACEMENT_BINARY_CONTAINED, 32U, binary_size,
             binary_size, 0U);
  FillRegion(&actual->regions[1], MTFS_SENTINEL_REGION_INTERPRETER,
             MTFS_SENTINEL_PLACEMENT_CALLER_RELATIVE, 32U,
             kInterpreterLogical, MTFS_RA8P1_ETHOSU_INTERPRETER_SIZE,
             kInterpreterOffset);
  FillRegion(&actual->regions[2], MTFS_SENTINEL_REGION_RESOLVER,
             MTFS_SENTINEL_PLACEMENT_CALLER_RELATIVE, 32U,
             kResolverLogical, MTFS_RA8P1_ETHOSU_RESOLVER_SIZE,
             kResolverOffset);
  FillRegion(&actual->regions[3], MTFS_SENTINEL_REGION_TENSOR_ARENA,
             MTFS_SENTINEL_PLACEMENT_CALLER_RELATIVE, 32U,
             MTFS_RA8P1_ETHOSU_ARENA_SIZE, MTFS_RA8P1_ETHOSU_ARENA_SIZE,
             kArenaOffset);
  FillRegion(&actual->regions[4], MTFS_SENTINEL_REGION_PROVIDER_CONTEXT,
             MTFS_SENTINEL_PLACEMENT_CALLER_RELATIVE, 32U,
             MTFS_RA8P1_ETHOSU_CONTEXT_SIZE, MTFS_RA8P1_ETHOSU_CONTEXT_SIZE,
             kContextOffset);
  FillRegion(&actual->regions[5], MTFS_SENTINEL_REGION_ACCELERATOR_CONTEXT,
             MTFS_SENTINEL_PLACEMENT_PROVIDER_ASSIGNED, 8U,
             kAcceleratorContext, kAcceleratorContext, 0U);
  FillRegion(&actual->regions[6], MTFS_SENTINEL_REGION_SEMAPHORE_POOL,
             MTFS_SENTINEL_PLACEMENT_PROVIDER_ASSIGNED, 4U,
             kSemaphorePool, kSemaphorePool, 0U);
  FillRegion(&actual->regions[7], MTFS_SENTINEL_REGION_PROVIDER_SYNC,
             MTFS_SENTINEL_PLACEMENT_PROVIDER_ASSIGNED, 4U,
             kProviderSync, kProviderSync, 0U);
  actual->region_count = 8U;
  mtfs_error_t status = mtfs_sentinel_sha256(binary, binary_size,
                                              actual->runtime_binary_hash);
  if (status != MTFS_OK) {
    SetDiagnostic(target, 3U, status, 0U, 0U);
    return status;
  }
  target->diagnostics.model_bytes = binary_size;
  SetDiagnostic(target, 10U, 0, 0U, 0U);
  return MTFS_OK;
}

mtfs_error_t EnsureGlobalOpen(mtfs_ra8p1_tflm_ethosu_t* target) {
  if (g_global.open) return MTFS_OK;
  if (g_rm_ethosu0.p_api->open(g_rm_ethosu0.p_ctrl,
                               g_rm_ethosu0.p_cfg) != FSP_SUCCESS) {
    ++g_global.open_failures;
    ++target->diagnostics.global_open_failures;
    return MTFS_ERROR_NOT_READY;
  }
  ethosu_set_basep_cache_mask(&g_ethosu0, kAllModelBasePointers,
                              kAllModelBasePointers);
  g_global.open = true;
  ++g_global.opens;
  ++target->diagnostics.global_opens;
  return MTFS_OK;
}

bool TensorContractMatches(const TfLiteTensor* tensor, uint32_t numerator,
                           uint32_t shift, int8_t zero_point) {
  if (tensor == nullptr || tensor->type != kTfLiteInt8 ||
      tensor->dims == nullptr || tensor->dims->size != 2 ||
      tensor->dims->data[0] != 1 || tensor->dims->data[1] != 24 ||
      shift > 31U || numerator == 0U || tensor->params.zero_point != zero_point) {
    return false;
  }
  const float expected = static_cast<float>(numerator) /
                         static_cast<float>(UINT32_C(1) << shift);
  uint32_t expected_bits = 0U;
  uint32_t actual_bits = 0U;
  std::memcpy(&expected_bits, &expected, sizeof(expected_bits));
  std::memcpy(&actual_bits, &tensor->params.scale, sizeof(actual_bits));
  return expected_bits == actual_bits;
}

void DestroySession(mtfs_ra8p1_tflm_ethosu_t* target, Session* session) {
  if (session == nullptr) return;
  if (session->interpreter_constructed) {
    session->interpreter->~MicroInterpreter();
    session->interpreter_constructed = false;
  }
  if (session->resolver_constructed) {
    session->resolver->~Resolver();
    session->resolver_constructed = false;
  }
  if (g_global.active_sessions != 0U) --g_global.active_sessions;
  target->session = nullptr;
}

mtfs_error_t InstallRuntime(void* opaque, const uint8_t* binary,
                            uint32_t binary_size, void* copy_memory,
                            uint32_t copy_size,
                            const mtfs_sentinel_runtime_info_t* runtime) {
  auto* target = static_cast<mtfs_ra8p1_tflm_ethosu_t*>(opaque);
  if (target == nullptr || binary == nullptr || copy_memory == nullptr ||
      runtime == nullptr || target->session != nullptr ||
      copy_size != MTFS_RA8P1_ETHOSU_PERSISTENT_SIZE ||
      (reinterpret_cast<uintptr_t>(copy_memory) & 31U) != 0U ||
      runtime->model_format != MTFS_SENTINEL_MODEL_FORMAT_RA_TFLM_ETHOSU ||
      runtime->model_version != 2U ||
      runtime->runtime_abi != MTFS_RA8P1_ETHOSU_RUNTIME_ABI ||
      runtime->runtime_variant != MTFS_RA8P1_ETHOSU_RUNTIME_VARIANT ||
      runtime->runtime_extra != MTFS_RA8P1_ETHOSU_RUNTIME_EXTRA) {
    return MTFS_ERROR_INVALID_ARGUMENT;
  }
  const tflite::Model* model = nullptr;
  if (!ValidateModel(binary, binary_size, &model)) {
    SetDiagnostic(target, 21U, 0, 1U, 0U);
    return MTFS_ERROR_UNSUPPORTED_FORMAT;
  }
  auto* memory = static_cast<uint8_t*>(copy_memory);
  std::memset(memory, 0, copy_size);
  auto* session = new (memory + kContextOffset) Session{};
  session->persistent = memory;
  session->model = model;
  target->session = session;
  SCB_CleanDCache_by_Addr(const_cast<uint8_t*>(binary),
                          static_cast<int32_t>(binary_size));
  ++target->diagnostics.cache_model_cleans;
  mtfs_error_t status = EnsureGlobalOpen(target);
  if (status != MTFS_OK) {
    SetDiagnostic(target, 22U, status, 0U, 0U);
    target->session = nullptr;
    return status;
  }
  if (g_global.active_sessions != 0U) {
    SetDiagnostic(target, 23U, 0, 0U, g_global.active_sessions);
    target->session = nullptr;
    return MTFS_ERROR_INVALID_STATE;
  }
  session->resolver = new (memory + kResolverOffset) Resolver();
  session->resolver_constructed = true;
  if (session->resolver->AddEthosU() != kTfLiteOk) {
    SetDiagnostic(target, 24U, 0, kTfLiteOk, 1U);
    DestroySession(target, session);
    return MTFS_ERROR_NOT_SUPPORTED;
  }
  session->interpreter = new (memory + kInterpreterOffset)
      tflite::MicroInterpreter(model, *session->resolver,
                               memory + kArenaOffset,
                               MTFS_RA8P1_ETHOSU_ARENA_SIZE);
  session->interpreter_constructed = true;
  if (session->interpreter->initialization_status() != kTfLiteOk ||
      session->interpreter->AllocateTensors() != kTfLiteOk) {
    SetDiagnostic(target, 25U, 0, kTfLiteOk, 1U);
    DestroySession(target, session);
    return MTFS_ERROR_BUFFER_TOO_SMALL;
  }
  session->arena_used = session->interpreter->arena_used_bytes();
  if (session->arena_used > MTFS_RA8P1_ETHOSU_ARENA_SIZE ||
      session->interpreter->inputs_size() != 1U ||
      session->interpreter->outputs_size() != 1U) {
    SetDiagnostic(target, 26U, 0, MTFS_RA8P1_ETHOSU_ARENA_SIZE,
                  session->arena_used);
    DestroySession(target, session);
    return MTFS_ERROR_UNSUPPORTED_FORMAT;
  }
  session->input = session->interpreter->input(0U);
  session->output = session->interpreter->output(0U);
  if (!TensorContractMatches(session->input, runtime->input_scale_numerator,
                             runtime->input_scale_shift,
                             runtime->input_zero_point) ||
      !TensorContractMatches(session->output, runtime->output_scale_numerator,
                             runtime->output_scale_shift,
                             runtime->output_zero_point)) {
    SetDiagnostic(target, 27U, 0, 1U, 0U);
    DestroySession(target, session);
    return MTFS_ERROR_UNSUPPORTED_FORMAT;
  }
  ++g_global.active_sessions;
  target->diagnostics.arena_used = session->arena_used;
  ++target->diagnostics.installs;
  SetDiagnostic(target, 30U, 0, 0U, 0U);
  return MTFS_OK;
}

mtfs_error_t InferRuntime(void* opaque, const int8_t input[24],
                          int8_t output[24], uint32_t timeout_ms) {
  auto* target = static_cast<mtfs_ra8p1_tflm_ethosu_t*>(opaque);
  auto* session = target == nullptr ? nullptr :
                  static_cast<Session*>(target->session);
  if (session == nullptr || input == nullptr || output == nullptr ||
      timeout_ms == 0U || !session->interpreter_constructed) {
    return MTFS_ERROR_INVALID_STATE;
  }
  /* TFLM may reuse the input allocation after every invocation. */
  std::memcpy(session->input->data.int8, input, kFeatureCount);
  ++target->diagnostics.invokes;
  if (session->interpreter->Invoke() != kTfLiteOk) {
    ++target->diagnostics.invoke_failures;
    return MTFS_ERROR_IO;
  }
  std::memcpy(output, session->output->data.int8, kFeatureCount);
  return MTFS_OK;
}

mtfs_error_t CloseRuntime(void* opaque) {
  auto* target = static_cast<mtfs_ra8p1_tflm_ethosu_t*>(opaque);
  auto* session = target == nullptr ? nullptr :
                  static_cast<Session*>(target->session);
  if (session == nullptr) return MTFS_ERROR_INVALID_STATE;
  DestroySession(target, session);
  ++target->diagnostics.closes;
  return MTFS_OK;
}

void ZeroizeRuntime(void*, void* address, uint32_t size) {
  if (address == nullptr || size == 0U) return;
  volatile uint8_t* destination = static_cast<volatile uint8_t*>(address);
  for (uint32_t i = 0U; i < size; ++i) destination[i] = 0U;
  SCB_CleanDCache_by_Addr(address, static_cast<int32_t>(size));
  __DSB();
}

const mtfs_sentinel_npu_provider_ops_t kProviderOps = {
    InspectRuntime, InstallRuntime, InferRuntime, CloseRuntime,
    [](void* opaque, uint32_t timeout_ms) -> mtfs_error_t {
      auto* target = static_cast<mtfs_ra8p1_tflm_ethosu_t*>(opaque);
      return target != nullptr && target->lock != nullptr ?
          target->lock(target->callback_context, timeout_ms) :
          MTFS_ERROR_INVALID_STATE;
    },
    [](void* opaque) {
      auto* target = static_cast<mtfs_ra8p1_tflm_ethosu_t*>(opaque);
      if (target != nullptr && target->unlock != nullptr)
        target->unlock(target->callback_context);
    },
    ZeroizeRuntime};

}  // namespace

extern "C" mtfs_error_t mtfs_ra8p1_tflm_ethosu_provider_config(
    mtfs_ra8p1_tflm_ethosu_t* target, mtfs_sentinel_npu_lock_fn lock,
    mtfs_sentinel_npu_unlock_fn unlock, void* callback_context,
    mtfs_sentinel_npu_provider_config_t* config) {
  if (target == nullptr || lock == nullptr || unlock == nullptr ||
      config == nullptr || target->session != nullptr) {
    return MTFS_ERROR_INVALID_ARGUMENT;
  }
  std::memset(target, 0, sizeof(*target));
  target->lock = lock;
  target->unlock = unlock;
  target->callback_context = callback_context;
  std::memset(config, 0, sizeof(*config));
  config->api_version = MTFS_SENTINEL_NPU_PROVIDER_API_VERSION;
  config->struct_size = sizeof(*config);
  config->provider_id = MTFS_SENTINEL_PROVIDER_RA_TFLM_ETHOSU;
  config->accelerator_id = MTFS_SENTINEL_ACCELERATOR_ETHOS_U55;
  config->ops = &kProviderOps;
  config->target = target;
  return MTFS_OK;
}

extern "C" mtfs_error_t mtfs_ra8p1_tflm_ethosu_global_shutdown(void) {
  if (g_global.active_sessions != 0U) return MTFS_ERROR_INVALID_STATE;
  if (!g_global.open) return MTFS_OK;
  if (g_rm_ethosu0.p_api->close(g_rm_ethosu0.p_ctrl) != FSP_SUCCESS)
    return MTFS_ERROR_IO;
  g_global.open = false;
  return MTFS_OK;
}

#else
typedef int mtfs_ra8p1_tflm_ethosu_disabled_translation_unit_t;
#endif
