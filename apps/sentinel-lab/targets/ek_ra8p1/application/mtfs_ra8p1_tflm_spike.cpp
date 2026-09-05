#include "mtfs_ra8p1_tflm_spike.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "bsp_api.h"
#include "common_data.h"
#include "ff.h"
#include "flatbuffers/flatbuffers.h"
#include "mtfs_ra8p1_ethosu_hooks.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

using UB = unsigned char;
extern "C" int tm_printf(UB* format, ...);
extern "C" uint64_t mtfs_ra8p1_benchmark_clock_us(void* context);
extern "C" uint32_t mtfs_ra8p1_heap_call_count(void);

namespace {

constexpr std::size_t kFeatureCount = 24U;
constexpr std::size_t kModelCapacity = 8U * 1024U;
constexpr std::size_t kArenaCapacity = 16U * 1024U;
constexpr int32_t kDecisionThresholdQ8 = 6;
constexpr uint8_t kAllModelBasePointers = 0x1fU;

struct Variant {
  const char* name;
  const char* path;
  const int8_t* expected_raw;
};

alignas(32) uint8_t g_model_buffer[kModelCapacity];
alignas(32) uint8_t g_tensor_arena[kArenaCapacity];
alignas(tflite::MicroInterpreter)
    uint8_t g_interpreter_storage[sizeof(tflite::MicroInterpreter)];
using Resolver = tflite::MicroMutableOpResolver<1>;
alignas(Resolver) uint8_t g_resolver_storage[sizeof(Resolver)];

constexpr int8_t kExpectedA[kFeatureCount] = {
    4, 2, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1,
    2, 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
constexpr int8_t kExpectedB[kFeatureCount] = {
    8, 2, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1,
    2, 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};

constexpr Variant kVariants[] = {
    {"A", "0:/SRA_A.TFL", kExpectedA},
    {"B", "0:/SRA_B.TFL", kExpectedB},
    {"A-reopen", "0:/SRA_A.TFL", kExpectedA},
};

void SecureZero(void* pointer, std::size_t size) {
  const std::size_t length = size;
  volatile uint8_t* destination = static_cast<volatile uint8_t*>(pointer);
  while (size-- != 0U) {
    *destination++ = 0U;
  }
  SCB_CleanDCache_by_Addr(static_cast<uint32_t*>(pointer),
                          static_cast<int32_t>(length));
}

bool IsZero(const void* pointer, std::size_t size) {
  const volatile uint8_t* source =
      static_cast<const volatile uint8_t*>(pointer);
  uint8_t combined = 0U;
  while (size-- != 0U) {
    combined = static_cast<uint8_t>(combined | *source++);
  }
  return combined == 0U;
}

bool LoadModel(const char* path, std::size_t* loaded_size) {
  FIL file{};
  UINT received = 0U;
  FRESULT result = f_open(&file, path, FA_READ | FA_OPEN_EXISTING);
  if (result != FR_OK) {
    tm_printf((UB*)"[RA0.1] load=FAIL path=%s fatfs=%u\n", (UB*)path,
              static_cast<unsigned int>(result));
    return false;
  }
  const FSIZE_t file_size = f_size(&file);
  if (file_size == 0U || file_size > kModelCapacity) {
    (void)f_close(&file);
    tm_printf((UB*)"[RA0.1] load=FAIL path=%s size=%u capacity=%u\n",
              (UB*)path, static_cast<unsigned int>(file_size),
              static_cast<unsigned int>(kModelCapacity));
    return false;
  }
  result = f_read(&file, g_model_buffer, static_cast<UINT>(file_size),
                  &received);
  const FRESULT close_result = f_close(&file);
  if (result != FR_OK || close_result != FR_OK || received != file_size) {
    tm_printf((UB*)"[RA0.1] load=FAIL path=%s read=%u close=%u bytes=%u/%u\n",
              (UB*)path, static_cast<unsigned int>(result),
              static_cast<unsigned int>(close_result), received,
              static_cast<unsigned int>(file_size));
    return false;
  }
  *loaded_size = static_cast<std::size_t>(file_size);
  return true;
}

bool ValidateModel(std::size_t model_size, const tflite::Model** model) {
  flatbuffers::Verifier verifier(g_model_buffer, model_size);
  if (!tflite::VerifyModelBuffer(verifier)) {
    return false;
  }
  *model = tflite::GetModel(g_model_buffer);
  if (*model == nullptr || (*model)->version() != TFLITE_SCHEMA_VERSION ||
      (*model)->subgraphs() == nullptr || (*model)->subgraphs()->size() != 1U ||
      (*model)->operator_codes() == nullptr ||
      (*model)->operator_codes()->size() != 1U) {
    return false;
  }
  const tflite::OperatorCode* code = (*model)->operator_codes()->Get(0U);
  const flatbuffers::String* custom = code == nullptr ? nullptr :
      code->custom_code();
  const tflite::SubGraph* graph = (*model)->subgraphs()->Get(0U);
  return code != nullptr && code->builtin_code() == tflite::BuiltinOperator_CUSTOM &&
      custom != nullptr && custom->size() == 7U &&
      std::memcmp(custom->c_str(), "ethos-u", 7U) == 0 && graph != nullptr &&
      graph->operators() != nullptr && graph->operators()->size() == 1U;
}

bool IsFeatureTensor(const TfLiteTensor* tensor) {
  return tensor != nullptr && tensor->type == kTfLiteInt8 &&
      tensor->dims != nullptr && tensor->dims->size == 2 &&
      tensor->dims->data[0] == 1 &&
      tensor->dims->data[1] == static_cast<int>(kFeatureCount);
}

int32_t ScoreQ8(const int8_t* output) {
  int32_t sum = 0;
  for (std::size_t index = 0U; index < kFeatureCount; ++index) {
    const int32_t q4 = static_cast<int32_t>(output[index]) - 1;
    sum += q4 * q4;
  }
  return sum / static_cast<int32_t>(kFeatureCount);
}

int RunVariant(const Variant& variant, uint32_t iterations,
               uint32_t heartbeat_before, volatile uint32_t* heartbeat,
               uint32_t sequence) {
  const tflite::Model* model = nullptr;
  Resolver* resolver = nullptr;
  tflite::MicroInterpreter* interpreter = nullptr;
  TfLiteTensor* input = nullptr;
  TfLiteTensor* output = nullptr;
  std::size_t model_size = 0U;
  std::size_t arena_used = 0U;
  uint32_t start_cycles = 0U;
  uint32_t elapsed_cycles = 0U;
  uint64_t start_us = 0U;
  uint64_t elapsed_us = 0U;
  int32_t score_q8 = 0;
  int32_t max_raw_error = 0;
  uint8_t npu_open = 0U;
  uint8_t npu_closed = 0U;
  int status = -1;

  SecureZero(g_model_buffer, sizeof(g_model_buffer));
  SecureZero(g_tensor_arena, sizeof(g_tensor_arena));
  SecureZero(g_interpreter_storage, sizeof(g_interpreter_storage));
  SecureZero(g_resolver_storage, sizeof(g_resolver_storage));

  if (!LoadModel(variant.path, &model_size) ||
      !ValidateModel(model_size, &model)) {
    tm_printf((UB*)"[RA0.1] variant=%s model-validate=FAIL\n",
              (UB*)variant.name);
    goto cleanup;
  }
  SCB_CleanDCache_by_Addr(reinterpret_cast<uint32_t*>(g_model_buffer),
                          static_cast<int32_t>(model_size));
  resolver = new (g_resolver_storage) Resolver();
  if (resolver->AddEthosU() != kTfLiteOk) {
    tm_printf((UB*)"[RA0.1] variant=%s resolver=FAIL\n", (UB*)variant.name);
    goto cleanup;
  }
  if (g_rm_ethosu0.p_api->open(g_rm_ethosu0.p_ctrl,
                               g_rm_ethosu0.p_cfg) != FSP_SUCCESS) {
    tm_printf((UB*)"[RA0.1] variant=%s npu-open=FAIL\n", (UB*)variant.name);
    goto cleanup;
  }
  npu_open = 1U;
  ethosu_set_basep_cache_mask(&g_ethosu0, kAllModelBasePointers,
                              kAllModelBasePointers);
  interpreter = new (g_interpreter_storage) tflite::MicroInterpreter(
      model, *resolver, g_tensor_arena, sizeof(g_tensor_arena));
  if (interpreter->initialization_status() != kTfLiteOk ||
      interpreter->AllocateTensors() != kTfLiteOk) {
    tm_printf((UB*)"[RA0.1] variant=%s allocate=FAIL\n", (UB*)variant.name);
    goto cleanup;
  }
  arena_used = interpreter->arena_used_bytes();
  if (interpreter->inputs_size() != 1U || interpreter->outputs_size() != 1U) {
    tm_printf((UB*)"[RA0.1] variant=%s io-count=FAIL\n", (UB*)variant.name);
    goto cleanup;
  }
  input = interpreter->input(0U);
  output = interpreter->output(0U);
  if (!IsFeatureTensor(input) || !IsFeatureTensor(output)) {
    tm_printf((UB*)"[RA0.1] variant=%s io-shape=FAIL\n", (UB*)variant.name);
    goto cleanup;
  }
  start_us = mtfs_ra8p1_benchmark_clock_us(nullptr);
  start_cycles = DWT->CYCCNT;
  for (uint32_t iteration = 0U; iteration < iterations; ++iteration) {
    /* The arena planner may reuse an input buffer after one invocation. */
    std::memset(input->data.int8, -1, kFeatureCount);
    if (interpreter->Invoke() != kTfLiteOk) {
      tm_printf((UB*)"[RA0.1] variant=%s invoke=FAIL iteration=%u\n",
                (UB*)variant.name, iteration);
      goto cleanup;
    }
    for (std::size_t index = 0U; index < kFeatureCount; ++index) {
      int32_t difference = static_cast<int32_t>(output->data.int8[index]) -
          static_cast<int32_t>(variant.expected_raw[index]);
      if (difference < 0) difference = -difference;
      if (difference > max_raw_error) max_raw_error = difference;
    }
  }
  elapsed_cycles = DWT->CYCCNT - start_cycles;
  elapsed_us = mtfs_ra8p1_benchmark_clock_us(nullptr) - start_us;
  score_q8 = ScoreQ8(output->data.int8);
  if (max_raw_error != 0) {
    tm_printf((UB*)"[RA0.1] variant=%s reference=FAIL max-raw-error=%d\n",
              (UB*)variant.name, max_raw_error);
    goto cleanup;
  }
  tm_printf((UB*)"[RA0.1] variant=%s seq=%u PASS model-bytes=%u schema=%u "
            "custom-op=ethos-u iterations=%u score-q8=%d decision=%u "
            "max-raw-error=%d cycles=%u us=%u arena-used=%u/%u heartbeat=%u\n",
            (UB*)variant.name, sequence, static_cast<unsigned int>(model_size),
            static_cast<unsigned int>(model->version()), iterations, score_q8,
            score_q8 >= kDecisionThresholdQ8 ? 1U : 0U, max_raw_error,
            elapsed_cycles, static_cast<unsigned int>(elapsed_us),
            static_cast<unsigned int>(arena_used),
            static_cast<unsigned int>(kArenaCapacity),
            heartbeat == nullptr ? 0U : *heartbeat - heartbeat_before);
  status = 0;

cleanup:
  if (interpreter != nullptr) {
    interpreter->~MicroInterpreter();
  }
  if (npu_open != 0U) {
    if (g_rm_ethosu0.p_api->close(g_rm_ethosu0.p_ctrl) != FSP_SUCCESS) {
      tm_printf((UB*)"[RA0.1] variant=%s npu-close=FAIL\n",
                (UB*)variant.name);
      status = -1;
    } else {
      npu_closed = 1U;
    }
  }
  if (resolver != nullptr) {
    resolver->~Resolver();
  }
  SecureZero(g_tensor_arena, sizeof(g_tensor_arena));
  SecureZero(g_model_buffer, sizeof(g_model_buffer));
  SecureZero(g_interpreter_storage, sizeof(g_interpreter_storage));
  SecureZero(g_resolver_storage, sizeof(g_resolver_storage));
  const bool zeroized = IsZero(g_tensor_arena, sizeof(g_tensor_arena)) &&
      IsZero(g_model_buffer, sizeof(g_model_buffer)) &&
      IsZero(g_interpreter_storage, sizeof(g_interpreter_storage)) &&
      IsZero(g_resolver_storage, sizeof(g_resolver_storage));
  tm_printf((UB*)"[RA0.1] variant=%s destroy=PASS close=%s zeroize=%s\n",
            (UB*)variant.name,
            npu_open == 0U ? "N/A" : (npu_closed != 0U ? "PASS" : "FAIL"),
            zeroized ? "PASS" : "FAIL");
  return status == 0 && zeroized ? 0 : -1;
}

}  // namespace

extern "C" int mtfs_ra8p1_tflm_spike_run(uint32_t iterations,
    uint32_t heartbeat_before, volatile uint32_t* heartbeat) {
  mtfs_ra8p1_ethosu_hook_diagnostics_t diagnostics{};
  const uint32_t heap_before = mtfs_ra8p1_heap_call_count();
  int result = 0;

  if (iterations == 0U) {
    return -1;
  }
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  mtfs_ra8p1_ethosu_hook_diagnostics_reset();
  tm_printf((UB*)"[RA0.1] runtime-load begin variants=A/B/A-reopen "
            "model-capacity=%u arena-capacity=%u interpreter-bytes=%u "
            "resolver-bytes=%u dcache=%s cache-policy=model-clean+base-0x1f\n",
            static_cast<unsigned int>(kModelCapacity),
            static_cast<unsigned int>(kArenaCapacity),
            static_cast<unsigned int>(sizeof(tflite::MicroInterpreter)),
            static_cast<unsigned int>(sizeof(Resolver)),
            (SCB->CCR & SCB_CCR_DC_Msk) != 0U ? "ON" : "OFF");
  for (uint32_t index = 0U;
       index < static_cast<uint32_t>(sizeof(kVariants) / sizeof(kVariants[0]));
       ++index) {
    const uint32_t before = heartbeat == nullptr ? heartbeat_before : *heartbeat;
    if (RunVariant(kVariants[index], iterations, before, heartbeat,
                   index + 1U) != 0) {
      result = -1;
      break;
    }
  }
  mtfs_ra8p1_ethosu_hook_diagnostics_get(&diagnostics);
  const uint32_t heap_calls = mtfs_ra8p1_heap_call_count() - heap_before;
  if (heap_calls != 0U) {
    result = -1;
  }
  tm_printf((UB*)"[RA0.1] semaphore fixed-pool=2 creates=%u create-fail=%u "
            "destroys=%u peak=%u takes=%u gives=%u wfe=%u wait-us=%u "
            "timeouts=%u heap-calls=%u heap=%s\n",
            diagnostics.creates, diagnostics.create_failures,
            diagnostics.destroys, diagnostics.peak_in_use,
            diagnostics.takes, diagnostics.gives, diagnostics.wfe_calls,
            static_cast<unsigned int>(diagnostics.wait_us),
            diagnostics.timeouts, heap_calls,
            heap_calls == 0U ? "NONE" : "USED");
  return result;
}
