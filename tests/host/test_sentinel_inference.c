#include "test_sentinel_inference.h"

#include <stdint.h>
#include <string.h>

#include "mtfs_sentinel_inference.h"
#include "mtfs_sentinel_npu_provider.h"
#include "mtfs_sentinel_sha256.h"
#if MTFS_ENABLE_SEALED_MODEL
#include "mtfs_sentinel_sealed_adapter.h"
#endif

#define TEST_BUNDLE_SIZE (1762U)
#define TEST_DUAL_BUNDLE_SIZE (2194U)

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8U);
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8U);
    p[2] = (uint8_t)(value >> 16U); p[3] = (uint8_t)(value >> 24U);
}

static void put64(uint8_t *p, uint64_t value)
{
    put32(p, (uint32_t)value); put32(p + 4U, (uint32_t)(value >> 32U));
}

static void directory(uint8_t *bundle, uint32_t index, uint16_t type,
    uint32_t offset, uint32_t length, uint32_t alignment, uint32_t provider)
{
    uint8_t *entry = bundle + 32U + index * 32U;
    put16(entry, type); put16(entry + 2U, 1U);
    put32(entry + 4U, offset); put32(entry + 8U, length);
    put32(entry + 12U, alignment); put32(entry + 16U, provider);
}

static void make_bundle(uint8_t bundle[TEST_BUNDLE_SIZE])
{
    uint8_t *compat, *normalization, *decision, *runtime, *model;
    uint32_t i;
    static const uint8_t hash[32] = {
        0x01,0xb0,0x04,0x05,0x33,0x49,0x1d,0x3f,
        0x2d,0x07,0x24,0x8b,0x34,0x59,0x09,0x44,
        0x7b,0xe0,0x04,0xd4,0xfc,0xd8,0xfc,0x94,
        0x71,0x9e,0xc2,0x8d,0x95,0x50,0x94,0xd8
    };
    static const uint16_t dimensions[5] = {24U,12U,4U,12U,24U};
    (void)memset(bundle, 0, TEST_BUNDLE_SIZE);
    (void)memcpy(bundle, "MTFSSB1", 7U);
    put16(bundle + 8U, 1U); put16(bundle + 10U, 192U);
    put32(bundle + 12U, TEST_BUNDLE_SIZE); put16(bundle + 16U, 5U);
    put16(bundle + 18U, 32U);
    directory(bundle, 0U, 1U, 192U, 128U, 4U, 0U);
    directory(bundle, 1U, 2U, 320U, 304U, 8U, 0U);
    directory(bundle, 2U, 3U, 624U, 32U, 8U, 0U);
    directory(bundle, 3U, 0x0100U, 656U, 1104U, 4U, 0x43505552U);
    directory(bundle, 4U, 4U, 1760U, 2U, 4U, 0U);
    compat = bundle + 192U;
    put16(compat, 1U); put16(compat + 2U, 1U);
    put16(compat + 4U, 33U); (void)memcpy(compat + 8U, hash, 32U);
    put32(compat + 40U, 0x52413850U); put32(compat + 44U, 0x53504920U);
    put32(compat + 48U, 0x180c040cU); put32(compat + 52U, 123U);
    put32(compat + 56U, 1U); put32(compat + 60U, 0x43505520U);
    put32(compat + 64U, 0x534e5431U);
    (void)memcpy(compat + 72U, "mtfs-storage-sentinel-model-input", 33U);
    normalization = bundle + 320U;
    put16(normalization, 1U); put16(normalization + 2U, 24U);
    put16(normalization + 4U, 16U); put16(normalization + 6U, 20U);
    put16(normalization + 8U, 4U); put16(normalization + 10U, 1U);
    put16(normalization + 12U, 0xff80U); put16(normalization + 14U, 127U);
    for (i = 0U; i < 24U; ++i) put32(normalization + 208U + i * 4U, 1U << 20U);
    decision = bundle + 624U;
    put16(decision, 1U); put16(decision + 2U, 24U);
    put16(decision + 4U, 4U); put16(decision + 6U, 4U);
    put16(decision + 8U, 8U); put16(decision + 10U, 1U);
    put16(decision + 12U, 1U); decision[16] = 1U;
    decision[24] = 0x01U; decision[25] = 0xfeU;
    runtime = bundle + 656U;
    put16(runtime, 1U); put16(runtime + 2U, 1U);
    put32(runtime + 4U, 0x43505552U); put32(runtime + 8U, 0x43505520U);
    put32(runtime + 12U, 0x51414531U); put32(runtime + 16U, 1U);
    put32(runtime + 20U, 1U); put16(runtime + 24U, 24U);
    put16(runtime + 26U, 24U); runtime[28] = runtime[29] = 1U;
    put32(runtime + 32U, 1U); put32(runtime + 36U, 4U);
    put32(runtime + 40U, 1U); put32(runtime + 44U, 4U);
    put32(runtime + 48U, 4U); put32(runtime + 52U, 32U);
    put32(runtime + 56U, 48U);
    put32(runtime + 60U, 912U);
    put32(runtime + 160U, 192U);
    model = runtime + 192U;
    (void)memcpy(model, "MTFSQAE1", 8U); put16(model + 8U, 1U);
    put16(model + 10U, 4U);
    for (i = 0U; i < 5U; ++i) put16(model + 12U + i * 2U, dimensions[i]);
    model[22] = 7U; model[23] = 4U; model[24] = 11U; model[25] = 1U;
    bundle[1760] = '{'; bundle[1761] = '}';
}

static void make_npu_runtime(uint8_t *runtime)
{
    uint8_t *entry;
    put16(runtime, 2U); put16(runtime + 2U, 2U);
    put32(runtime + 4U, 0x4e505250U); put32(runtime + 8U, 0x4e505531U);
    put32(runtime + 12U, 0x564e4431U); put32(runtime + 16U, 1U);
    put32(runtime + 20U, 0x00080000U); put16(runtime + 24U, 24U);
    put16(runtime + 26U, 24U); runtime[28] = runtime[29] = 1U;
    put32(runtime + 32U, 1U); put32(runtime + 36U, 4U);
    put32(runtime + 40U, 1U); put32(runtime + 44U, 4U);
    put32(runtime + 48U, 16U); put32(runtime + 52U, 64U);
    put32(runtime + 56U, 0U); put32(runtime + 60U, 16U);
    put32(runtime + 160U, 384U); put32(runtime + 164U, 192U);
    put16(runtime + 168U, 3U); put16(runtime + 170U, 64U);
    put16(runtime + 172U, 192U); put16(runtime + 176U, 8U);
    entry = runtime + 192U;
    put16(entry, 1U); put16(entry + 2U, MTFS_SENTINEL_REGION_EXECUTABLE_COPY);
    put16(entry + 4U, MTFS_SENTINEL_PLACEMENT_CALLER_RELATIVE);
    put16(entry + 6U, 1U); put64(entry + 8U, 64U); put32(entry + 16U, 16U);
    put64(entry + 24U, 0U); put32(entry + 32U, MTFS_SENTINEL_REGION_LIFETIME_INSTANCE);
    put32(entry + 36U, 3U); put32(entry + 40U, 5U);
    put32(entry + 44U, MTFS_SENTINEL_REGION_REQUIRE_ZEROIZE |
        MTFS_SENTINEL_REGION_REQUIRE_EXCLUSIVE); put64(entry + 48U, 64U);
    entry += 64U;
    put16(entry, 1U); put16(entry + 2U, MTFS_SENTINEL_REGION_ACTIVATION);
    put16(entry + 4U, MTFS_SENTINEL_PLACEMENT_FIXED_ABSOLUTE);
    put16(entry + 6U, 1U); put64(entry + 8U, 56U); put32(entry + 16U, 8U);
    put64(entry + 24U, UINT64_C(0x342e0000));
    put32(entry + 32U, MTFS_SENTINEL_REGION_LIFETIME_INSTANCE);
    put32(entry + 36U, 2U); put32(entry + 40U, 3U);
    put32(entry + 44U, MTFS_SENTINEL_REGION_REQUIRE_ZEROIZE |
        MTFS_SENTINEL_REGION_REQUIRE_EXCLUSIVE |
        MTFS_SENTINEL_REGION_REQUIRE_INPUT_OUTPUT_SHARED); put64(entry + 48U, 56U);
    entry += 64U;
    put16(entry, 1U); put16(entry + 2U, MTFS_SENTINEL_REGION_PARAMETERS);
    put16(entry + 4U, MTFS_SENTINEL_PLACEMENT_BINARY_CONTAINED);
    put16(entry + 6U, 1U); put64(entry + 8U, 8U); put32(entry + 16U, 8U);
    put64(entry + 24U, 8U); put32(entry + 32U, MTFS_SENTINEL_REGION_LIFETIME_INSTANCE);
    put32(entry + 36U, 1U); put32(entry + 40U, 1U); put64(entry + 48U, 8U);
    (void)memset(runtime + 384U, 0xa5, 16U);
}

static void make_dual_bundle_ordered(
    uint8_t bundle[TEST_DUAL_BUNDLE_SIZE], int npu_first)
{
    uint8_t cpu_bundle[TEST_BUNDLE_SIZE];
    uint32_t cpu_offset = npu_first ? 1088U : 688U;
    uint32_t npu_offset = npu_first ? 688U : 1792U;
    make_bundle(cpu_bundle);
    (void)memset(bundle, 0, TEST_DUAL_BUNDLE_SIZE);
    (void)memcpy(bundle, "MTFSSB1", 7U);
    put16(bundle + 8U, 1U); put16(bundle + 10U, 224U);
    put32(bundle + 12U, TEST_DUAL_BUNDLE_SIZE); put16(bundle + 16U, 6U);
    put16(bundle + 18U, 32U);
    directory(bundle, 0U, 1U, 224U, 128U, 4U, 0U);
    directory(bundle, 1U, 2U, 352U, 304U, 8U, 0U);
    directory(bundle, 2U, 3U, 656U, 32U, 8U, 0U);
    directory(bundle, 3U, npu_first ? 0x0101U : 0x0100U,
        npu_first ? npu_offset : cpu_offset, npu_first ? 400U : 1104U,
        npu_first ? 16U : 4U,
        npu_first ? 0x4e505250U : 0x43505552U);
    directory(bundle, 4U, npu_first ? 0x0100U : 0x0101U,
        npu_first ? cpu_offset : npu_offset, npu_first ? 1104U : 400U,
        npu_first ? 4U : 16U,
        npu_first ? 0x43505552U : 0x4e505250U);
    directory(bundle, 5U, 4U, 2192U, 2U, 4U, 0U);
    (void)memcpy(bundle + 224U, cpu_bundle + 192U, 128U);
    (void)memcpy(bundle + 352U, cpu_bundle + 320U, 304U);
    (void)memcpy(bundle + 656U, cpu_bundle + 624U, 32U);
    (void)memcpy(bundle + cpu_offset, cpu_bundle + 656U, 1104U);
    (void)memcpy(bundle + 2192U, cpu_bundle + 1760U, 2U);
    put32(bundle + 224U + 60U, 0x4e505531U);
    make_npu_runtime(bundle + npu_offset);
}

static void make_dual_bundle(uint8_t bundle[TEST_DUAL_BUNDLE_SIZE])
{
    make_dual_bundle_ordered(bundle, 0);
}

static void make_feature(mtfs_sentinel_feature_v1_t *feature)
{
    uint32_t op;
    (void)memset(feature, 0, sizeof(*feature));
    feature->version = 1U; feature->struct_size = (uint16_t)sizeof(*feature);
    feature->validity_mask = MTFS_SENTINEL_VALID_REQUIRED;
    feature->sample_count = 1U;
    for (op = 0U; op < 3U; ++op) {
        feature->operation[op].timing_samples = 10U;
        feature->operation[op].latency_histogram[0] = 10U;
    }
}

typedef struct fake_npu
{
    uint8_t locked;
    uint8_t installed;
    uint8_t closed;
    uint8_t zeroized;
    uint8_t inspect_mismatch;
    uint8_t fail_install;
    uint8_t fail_infer;
    uint8_t fail_close;
    uint32_t lock_calls;
    uint32_t install_calls;
} fake_npu_t;

static mtfs_error_t fake_inspect(void *opaque, const uint8_t *binary,
    uint32_t binary_size, mtfs_sentinel_npu_actual_info_t *actual)
{
    (void)opaque;
    if (binary == NULL || binary_size != 16U || actual == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    (void)memset(actual, 0, sizeof(*actual));
    actual->runtime_abi = 0x00080000U;
    actual->copy_size = 64U; actual->copy_alignment = 16U;
    actual->parameters_offset = 8U; actual->parameters_logical_size = 8U;
    actual->parameters_storage_size = 8U;
    actual->activation_address = 0x342e0000U; actual->activation_size = 56U;
    actual->region_count = 3U;
    actual->regions[0].kind = MTFS_SENTINEL_REGION_EXECUTABLE_COPY;
    actual->regions[0].placement = MTFS_SENTINEL_PLACEMENT_CALLER_RELATIVE;
    actual->regions[0].alignment = 16U;
    actual->regions[0].logical_size = 64U;
    actual->regions[0].storage_size = 64U;
    actual->regions[1].kind = MTFS_SENTINEL_REGION_ACTIVATION;
    actual->regions[1].placement = MTFS_SENTINEL_PLACEMENT_FIXED_ABSOLUTE;
    actual->regions[1].alignment = 8U;
    actual->regions[1].logical_size = 56U;
    actual->regions[1].storage_size = 56U;
    actual->regions[1].address_or_offset = UINT64_C(0x342e0000);
    actual->regions[2].kind = MTFS_SENTINEL_REGION_PARAMETERS;
    actual->regions[2].placement = MTFS_SENTINEL_PLACEMENT_BINARY_CONTAINED;
    actual->regions[2].alignment = 8U;
    actual->regions[2].logical_size = 8U;
    actual->regions[2].storage_size = 8U;
    actual->regions[2].address_or_offset = 8U;
    if (((fake_npu_t *)opaque)->inspect_mismatch != 0U)
        ++actual->regions[0].storage_size;
    return MTFS_OK;
}

static mtfs_error_t fake_install(void *opaque, const uint8_t *binary,
    uint32_t binary_size, void *copy, uint32_t copy_size,
    const mtfs_sentinel_runtime_info_t *runtime)
{
    fake_npu_t *fake = opaque;
    if (!fake->locked || binary == NULL || binary_size != 16U ||
        copy == NULL || copy_size != 64U || runtime == NULL ||
        runtime->input_data_type != MTFS_SENTINEL_DATA_TYPE_INT8 ||
        runtime->input_zero_point != 0) return MTFS_ERROR_INVALID_STATE;
    ++fake->install_calls;
    (void)memset(copy, 0xa5, copy_size);
    if (fake->fail_install != 0U) return MTFS_ERROR_NOT_READY;
    fake->installed = 1U;
    return MTFS_OK;
}

static mtfs_error_t fake_infer(void *opaque, const int8_t input[24],
    int8_t output[24], uint32_t timeout_ms)
{
    fake_npu_t *fake = opaque;
    if (!fake->installed || timeout_ms == 0U) return MTFS_ERROR_INVALID_STATE;
    if (fake->fail_infer != 0U) return MTFS_ERROR_NOT_READY;
    (void)memcpy(output, input, 24U); return MTFS_OK;
}

static mtfs_error_t fake_close(void *opaque)
{
    fake_npu_t *fake = opaque;
    if (!fake->installed) return MTFS_ERROR_INVALID_STATE;
    if (fake->fail_close != 0U) return MTFS_ERROR_NOT_READY;
    fake->installed = 0U; fake->closed = 1U; return MTFS_OK;
}

static mtfs_error_t fake_lock(void *opaque, uint32_t timeout_ms)
{
    fake_npu_t *fake = opaque;
    if (fake->locked || timeout_ms == 0U) return MTFS_ERROR_INVALID_STATE;
    ++fake->lock_calls; fake->locked = 1U; return MTFS_OK;
}

static void fake_unlock(void *opaque) { ((fake_npu_t *)opaque)->locked = 0U; }

static void fake_zeroize(void *opaque, void *address, uint32_t size)
{
    fake_npu_t *fake = opaque;
    if ((uintptr_t)address != UINT32_C(0x342e0000))
        (void)memset(address, 0, size);
    fake->zeroized = 1U;
}

static uint32_t fake_cycle_count(void *opaque)
{
    uint32_t *cycles = opaque;
    return ++*cycles;
}

static int test_alias_pair(mtfs_test_t *test,
    const mtfs_sentinel_cpu_context_t *cpu, uint32_t pair)
{
    union { uint64_t alignment; uint8_t bytes[192]; } arena, before;
    int8_t *input = (int8_t *)(arena.bytes + 0U);
    int8_t *output = (int8_t *)(arena.bytes + 32U);
    void *work = arena.bytes + 64U;
    mtfs_sentinel_inference_result_t *result =
        (mtfs_sentinel_inference_result_t *)(void *)(arena.bytes + 128U);
    (void)memset(&arena, 0x5a, sizeof(arena));
    if (pair == 0U) output = input;
    else if (pair == 1U) work = input;
    else if (pair == 2U)
        result = (mtfs_sentinel_inference_result_t *)(void *)input;
    else if (pair == 3U) work = output;
    else if (pair == 4U)
        result = (mtfs_sentinel_inference_result_t *)(void *)output;
    else result = (mtfs_sentinel_inference_result_t *)work;
    before = arena;
    return MTFS_TEST_CHECK(test,
        mtfs_sentinel_cpu_infer(cpu, input, work,
            MTFS_SENTINEL_CPU_WORK_SIZE, output, result) ==
            MTFS_ERROR_INVALID_ARGUMENT &&
        memcmp(&arena, &before, sizeof(arena)) == 0,
        "reject every direct alias pair without modifying caller memory");
}

int test_sentinel_inference(mtfs_test_t *test)
{
    static const uint8_t abc_sha256[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    uint8_t sha256[32];
    uint8_t bytes[TEST_BUNDLE_SIZE], damaged[TEST_BUNDLE_SIZE], work[48];
    uint8_t dual_storage[TEST_DUAL_BUNDLE_SIZE + 31U];
    uint8_t dual_damaged_storage[TEST_DUAL_BUNDLE_SIZE + 31U];
    uint8_t reversed_storage[TEST_DUAL_BUNDLE_SIZE + 31U];
    uint8_t *dual = (uint8_t *)(((uintptr_t)dual_storage + 31U) &
        ~(uintptr_t)31U);
    uint8_t *dual_damaged = (uint8_t *)
        (((uintptr_t)dual_damaged_storage + 31U) & ~(uintptr_t)31U);
    uint8_t *reversed = (uint8_t *)
        (((uintptr_t)reversed_storage + 31U) & ~(uintptr_t)31U);
    mtfs_sentinel_bundle_policy_t policy = {MTFS_SENTINEL_INFERENCE_API_VERSION,
        (uint16_t)sizeof(policy),
        0x52413850U, 0x53504920U, 0x43505520U, 0x534e5431U, 123U, 4096U};
    mtfs_sentinel_bundle_t bundle;
    mtfs_sentinel_cpu_context_t cpu;
    mtfs_sentinel_inference_result_t result;
    mtfs_sentinel_score_interval_t score_interval;
    mtfs_sentinel_runtime_info_t runtime_info;
    mtfs_sentinel_runtime_region_info_t region_info;
    mtfs_sentinel_runtime_region_policy_t region_policy[3];
    mtfs_sentinel_runtime_policy_result_t policy_result;
    mtfs_sentinel_npu_context_t npu_context;
    mtfs_sentinel_npu_provider_config_t npu_config;
    mtfs_sentinel_npu_inference_profile_t npu_profile;
    fake_npu_t fake_npu;
    uint32_t fake_cycles;
    uint8_t npu_copy_storage[79];
    uint8_t *npu_copy = (uint8_t *)(((uintptr_t)npu_copy_storage + 15U) &
        ~(uintptr_t)15U);
    mtfs_sentinel_memory_plan_t memory_plan;
    mtfs_sentinel_normalization_t normalization;
    mtfs_sentinel_feature_v1_t feature;
    uint32_t raw[24];
    int8_t input[24], output[24], raw_output[24];
    uint32_t i, runtime_count;
    mtfs_error_t open_status;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_sha256("abc", 3U, sha256) == MTFS_OK &&
            memcmp(sha256, abc_sha256, sizeof(sha256)) == 0,
            "portable SHA-256 validates runtime binary identity")) return 1;
    make_bundle(bytes);
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(bytes, sizeof(bytes), &policy, &bundle) == MTFS_OK,
            "parse byte-decoded bundle with outer/inner policy")) return 1;
    if (!MTFS_TEST_CHECK(test, bundle.runtime_count == 1U && bundle.threshold_q8 == 1U,
            "expose bounded runtime and decision metadata")) return 1;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_runtime_count(&bundle, &runtime_count) == MTFS_OK &&
            runtime_count == 1U &&
            mtfs_sentinel_bundle_runtime_get(&bundle, 0U, &runtime_info) == MTFS_OK &&
            runtime_info.api_version == MTFS_SENTINEL_INFERENCE_API_VERSION &&
            runtime_info.struct_size == sizeof(runtime_info) &&
            runtime_info.runtime_type == MTFS_SENTINEL_RUNTIME_CPU_INT8 &&
            runtime_info.runtime_index == 0U &&
            runtime_info.provider_id == MTFS_SENTINEL_PROVIDER_CPU_REFERENCE &&
            runtime_info.binary_size == MTFS_SENTINEL_CPU_MODEL_BINARY_SIZE &&
            mtfs_sentinel_bundle_runtime_find(&bundle,
                MTFS_SENTINEL_PROVIDER_CPU_REFERENCE,
                MTFS_SENTINEL_OUTER_ACCELERATOR_CPU, &runtime_info) == MTFS_OK &&
            runtime_info.runtime_index == 0U,
            "enumerate, inspect, and strictly find the CPU runtime")) return 1;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_runtime_find(&bundle, 0U,
                MTFS_SENTINEL_OUTER_ACCELERATOR_CPU, &runtime_info) ==
                MTFS_ERROR_INVALID_ARGUMENT &&
            mtfs_sentinel_bundle_runtime_find(&bundle,
                MTFS_SENTINEL_PROVIDER_CPU_REFERENCE, 0U, &runtime_info) ==
                MTFS_ERROR_INVALID_ARGUMENT &&
            mtfs_sentinel_bundle_runtime_find(&bundle, 1U, 1U,
                &runtime_info) == MTFS_ERROR_NOT_FOUND &&
            mtfs_sentinel_bundle_runtime_get(&bundle, 1U, &runtime_info) ==
                MTFS_ERROR_OUT_OF_RANGE,
            "zero runtime IDs are invalid and exact misses are not found")) return 1;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_memory_plan(&bundle, &memory_plan) == MTFS_OK &&
            memory_plan.required_alignment == 4U &&
            memory_plan.persistent_offset[0] == 1764U &&
            memory_plan.scratch_offset == 1796U &&
            memory_plan.required_ram == 1844U,
            "derive CPU-only persistent and scratch placement")) return 1;
#if MTFS_ENABLE_SEALED_MODEL
    {
        mtfs_model_info_t outer;
        (void)memset(&outer, 0, sizeof(outer));
        outer.api_version = MTFS_MODEL_INFO_API_VERSION;
        outer.struct_size = (uint32_t)sizeof(outer);
        outer.target_id = 0x52413850U;
        outer.accelerator_id = MTFS_SENTINEL_OUTER_ACCELERATOR_CPU;
        outer.model_format = MTFS_MODEL_FORMAT_SENTINEL_BUNDLE_V1;
        outer.payload_size = sizeof(bytes);
        outer.required_ram = memory_plan.required_ram;
        if (!MTFS_TEST_CHECK(test,
                mtfs_sentinel_bundle_parse_model_payload(bytes, sizeof(bytes), &outer,
                    0x53504920U, 123U, &bundle) == MTFS_OK,
                "bind authenticated outer model identity to inner bundle")) return 1;
        outer.required_ram = memory_plan.required_ram - 1U;
        if (!MTFS_TEST_CHECK(test,
                mtfs_sentinel_bundle_parse_model_payload(bytes, sizeof(bytes), &outer,
                    0x53504920U, 123U, &bundle) == MTFS_ERROR_BUFFER_TOO_SMALL,
                "sealed adapter rejects descriptor-derived RAM under-allocation")) return 1;
        outer.required_ram = UINT64_MAX;
        if (!MTFS_TEST_CHECK(test,
                mtfs_sentinel_bundle_parse_model_payload(bytes, sizeof(bytes), &outer,
                    0x53504920U, 123U, &bundle) == MTFS_ERROR_OVERFLOW,
                "sealed adapter rejects outer RAM outside the V1 address contract"))
            return 1;
        outer.required_ram = memory_plan.required_ram;
        outer.target_id = 0x53544e36U;
        if (!MTFS_TEST_CHECK(test,
                mtfs_sentinel_bundle_parse_model_payload(bytes, sizeof(bytes), &outer,
                    0x53504920U, 123U, &bundle) == MTFS_ERROR_UNSUPPORTED_FORMAT,
                "sealed adapter rejects outer/inner target mismatch")) return 1;
    }
#endif
    make_dual_bundle(dual);
    policy.expected_accelerator_id = 0x4e505531U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(dual, TEST_DUAL_BUNDLE_SIZE,
                &policy, &bundle) == MTFS_OK &&
            mtfs_sentinel_bundle_runtime_count(&bundle, &runtime_count) == MTFS_OK &&
            runtime_count == 2U &&
            mtfs_sentinel_bundle_runtime_get(&bundle, 1U, &runtime_info) == MTFS_OK &&
            runtime_info.runtime_type == MTFS_SENTINEL_RUNTIME_NPU &&
            runtime_info.runtime_index == 1U &&
            runtime_info.provider_id == 0x4e505250U &&
            runtime_info.accelerator_id == 0x4e505531U &&
            runtime_info.model_format == 0x564e4431U &&
            runtime_info.model_version == 1U && runtime_info.runtime_abi == 0x00080000U &&
            runtime_info.input_dimension == 24U &&
            runtime_info.output_dimension == 24U &&
            runtime_info.input_data_type == MTFS_SENTINEL_DATA_TYPE_INT8 &&
            runtime_info.output_data_type == MTFS_SENTINEL_DATA_TYPE_INT8 &&
            runtime_info.input_zero_point == 0 &&
            runtime_info.output_zero_point == 0 &&
            runtime_info.input_scale_numerator == 1U &&
            runtime_info.input_scale_shift == 4U &&
            runtime_info.output_scale_numerator == 1U &&
            runtime_info.output_scale_shift == 4U &&
            runtime_info.required_alignment == 16U &&
            runtime_info.persistent_memory == 64U &&
            runtime_info.scratch_memory == 0U &&
            runtime_info.descriptor_version == 2U && runtime_info.region_count == 3U &&
            runtime_info.runtime_version_major == 8U &&
            runtime_info.binary == dual + 2176U && runtime_info.binary_size == 16U &&
            runtime_info.binary[0] == 0xa5U &&
            runtime_info.canonical_model_hash[0] == 0U &&
            runtime_info.runtime_binary_hash[0] == 0U &&
            runtime_info.conversion_manifest_hash[0] == 0U &&
            mtfs_sentinel_bundle_runtime_find(&bundle, 0x4e505250U,
                0x4e505531U, &runtime_info) == MTFS_OK &&
            runtime_info.runtime_index == 1U,
            "expose a target NPU runtime without target-side directory parsing")) return 1;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_runtime_region_get(&bundle, 1U, 0U,
                &region_info) == MTFS_OK &&
            region_info.kind == MTFS_SENTINEL_REGION_EXECUTABLE_COPY &&
            region_info.placement == MTFS_SENTINEL_PLACEMENT_CALLER_RELATIVE &&
            region_info.storage_size == 64U && region_info.address_or_offset == 0U &&
            mtfs_sentinel_bundle_runtime_region_get(&bundle, 1U, 1U,
                &region_info) == MTFS_OK &&
            region_info.kind == MTFS_SENTINEL_REGION_ACTIVATION &&
            region_info.placement == MTFS_SENTINEL_PLACEMENT_FIXED_ABSOLUTE &&
            region_info.address_or_offset == UINT64_C(0x342e0000) &&
            mtfs_sentinel_bundle_runtime_region_get(&bundle, 1U, 2U,
                &region_info) == MTFS_OK &&
            region_info.placement == MTFS_SENTINEL_PLACEMENT_BINARY_CONTAINED &&
            mtfs_sentinel_bundle_runtime_region_get(&bundle, 1U, 3U,
                &region_info) == MTFS_ERROR_NOT_FOUND,
            "expose authenticated NPU v2 regions without expanding board policy")) return 1;
    (void)memset(region_policy, 0, sizeof(region_policy));
    for (i = 0U; i < 3U; ++i) {
        region_policy[i].provider_id = 0x4e505250U;
        region_policy[i].accelerator_id = 0x4e505531U;
    }
    region_policy[0].kind = MTFS_SENTINEL_REGION_EXECUTABLE_COPY;
    region_policy[0].placement = MTFS_SENTINEL_PLACEMENT_CALLER_RELATIVE;
    region_policy[0].minimum_alignment = 16U;
    region_policy[0].address_limit = 64U;
    region_policy[0].maximum_storage_size = 64U;
    region_policy[0].allowed_install_access = 3U;
    region_policy[0].allowed_inference_access = 5U;
    region_policy[0].allowed_requirements = 5U;
    region_policy[0].policy_flags = MTFS_SENTINEL_REGION_POLICY_OWNED |
        MTFS_SENTINEL_REGION_POLICY_EXCLUSIVE |
        MTFS_SENTINEL_REGION_POLICY_ALLOW_ZEROIZE |
        MTFS_SENTINEL_REGION_POLICY_GLOBAL_SERIALIZATION;
    region_policy[1].kind = MTFS_SENTINEL_REGION_ACTIVATION;
    region_policy[1].placement = MTFS_SENTINEL_PLACEMENT_FIXED_ABSOLUTE;
    region_policy[1].minimum_alignment = 8U;
    region_policy[1].address_minimum = UINT64_C(0x342e0000);
    region_policy[1].address_limit = UINT64_C(0x342e0038);
    region_policy[1].maximum_storage_size = 56U;
    region_policy[1].allowed_install_access = 2U;
    region_policy[1].allowed_inference_access = 3U;
    region_policy[1].allowed_requirements = 21U;
    region_policy[1].policy_flags = MTFS_SENTINEL_REGION_POLICY_OWNED |
        MTFS_SENTINEL_REGION_POLICY_EXCLUSIVE |
        MTFS_SENTINEL_REGION_POLICY_ALLOW_ZEROIZE;
    region_policy[2].kind = MTFS_SENTINEL_REGION_PARAMETERS;
    region_policy[2].placement = MTFS_SENTINEL_PLACEMENT_BINARY_CONTAINED;
    region_policy[2].minimum_alignment = 8U;
    region_policy[2].address_minimum = 8U;
    region_policy[2].address_limit = 16U;
    region_policy[2].maximum_storage_size = 8U;
    region_policy[2].allowed_install_access = 1U;
    region_policy[2].allowed_inference_access = 1U;
    {
        static const mtfs_sentinel_npu_provider_ops_t fake_ops = {
            fake_inspect, fake_install, fake_infer, fake_close, fake_lock,
            fake_unlock, fake_zeroize
        };
        (void)memset(&fake_npu, 0, sizeof(fake_npu));
        (void)memset(&npu_config, 0, sizeof(npu_config));
        npu_config.api_version = MTFS_SENTINEL_NPU_PROVIDER_API_VERSION;
        npu_config.struct_size = (uint16_t)sizeof(npu_config);
        npu_config.provider_id = 0x4e505250U;
        npu_config.accelerator_id = 0x4e505531U;
        npu_config.ops = &fake_ops; npu_config.target = &fake_npu;
        (void)memset(input, 0, sizeof(input)); input[0] = 8;
        open_status = mtfs_sentinel_npu_open(&npu_context, &npu_config, &bundle, 1U,
            npu_copy, 64U, region_policy, 3U, 100U);
        if (!MTFS_TEST_CHECK(test, open_status != MTFS_ERROR_NOT_SUPPORTED,
                "common provider format/policy validation")) return 1;
        if (!MTFS_TEST_CHECK(test, open_status != MTFS_ERROR_UNSUPPORTED_FORMAT,
                "common provider runtime-info validation")) return 1;
        if (!MTFS_TEST_CHECK(test, open_status == MTFS_OK,
                "common provider opens after three-way validation")) return 1;
        if (!MTFS_TEST_CHECK(test,
                fake_npu.locked && fake_npu.installed &&
                mtfs_sentinel_npu_infer(&npu_context, input, output, 100U,
                    &result) == MTFS_OK && output[0] == 8 &&
                result.score_q8 == 0U && result.anomaly == 0U &&
                mtfs_sentinel_npu_close(&npu_context, 100U) == MTFS_OK &&
                !fake_npu.locked && fake_npu.closed && fake_npu.zeroized &&
                npu_copy[0] == 0U,
                "common provider validates, requantizes, infers, and zeroizes"))
            return 1;

        (void)memset(&fake_npu, 0, sizeof(fake_npu));
        fake_cycles = 0U;
        if (!MTFS_TEST_CHECK(test,
                mtfs_sentinel_npu_open(&npu_context, &npu_config, &bundle, 1U,
                    npu_copy, 64U, region_policy, 3U, 100U) == MTFS_OK &&
                mtfs_sentinel_npu_infer_profiled_detailed(&npu_context,
                    input, raw_output, output, 100U, &result, fake_cycle_count,
                    &fake_cycles, &npu_profile) == MTFS_OK &&
                npu_profile.attempted == 1U && npu_profile.completed == 1U &&
                npu_profile.input_requantize_cycles != 0U &&
                npu_profile.target_infer_cycles != 0U &&
                npu_profile.output_requantize_cycles != 0U &&
                npu_profile.score_decision_cycles != 0U &&
                npu_profile.total_cycles >=
                    npu_profile.input_requantize_cycles +
                    npu_profile.target_infer_cycles +
                    npu_profile.output_requantize_cycles +
                    npu_profile.score_decision_cycles &&
                mtfs_sentinel_npu_close(&npu_context, 100U) == MTFS_OK,
                "opt-in provider profiling reports each inference stage"))
            return 1;

        (void)memset(&fake_npu, 0, sizeof(fake_npu));
        (void)memset(npu_copy, 0x3c, 64U);
        fake_npu.inspect_mismatch = 1U;
        if (!MTFS_TEST_CHECK(test,
                mtfs_sentinel_npu_open(&npu_context, &npu_config, &bundle, 1U,
                    npu_copy, 64U, region_policy, 3U, 100U) ==
                    MTFS_ERROR_UNSUPPORTED_FORMAT &&
                fake_npu.lock_calls == 0U && fake_npu.install_calls == 0U &&
                npu_copy[0] == 0x3cU && npu_copy[63] == 0x3cU,
                "runtime mismatch is rejected before lock, install, or memory mutation"))
            return 1;

        (void)memset(&fake_npu, 0, sizeof(fake_npu));
        (void)memset(npu_copy, 0x3c, 64U);
        fake_npu.fail_install = 1U;
        if (!MTFS_TEST_CHECK(test,
                mtfs_sentinel_npu_open(&npu_context, &npu_config, &bundle, 1U,
                    npu_copy, 64U, region_policy, 3U, 100U) ==
                    MTFS_ERROR_NOT_READY &&
                fake_npu.lock_calls == 1U && fake_npu.install_calls == 1U &&
                !fake_npu.locked && fake_npu.zeroized &&
                npu_copy[0] == 0U && npu_copy[63] == 0U,
                "install failure zeroizes only owned COPY memory and releases the lock"))
            return 1;

        (void)memset(&fake_npu, 0, sizeof(fake_npu));
        (void)memset(output, 0x55, sizeof(output));
        (void)memset(&result, 0x66, sizeof(result));
        if (!MTFS_TEST_CHECK(test,
                mtfs_sentinel_npu_open(&npu_context, &npu_config, &bundle, 1U,
                    npu_copy, 64U, region_policy, 3U, 100U) == MTFS_OK,
                "provider opens for inference failure recovery")) return 1;
        fake_npu.fail_infer = 1U;
        if (!MTFS_TEST_CHECK(test,
                mtfs_sentinel_npu_infer(&npu_context, input, output, 100U,
                    &result) == MTFS_ERROR_NOT_READY &&
                (uint8_t)output[0] == 0x55U && (uint8_t)output[23] == 0x55U &&
                ((const uint8_t *)(const void *)&result)[0] == 0x66U &&
                npu_context.open && fake_npu.locked,
                "inference failure preserves outputs and keeps explicit close ownership"))
            return 1;
        fake_npu.fail_infer = 0U;
        fake_npu.fail_close = 1U;
        fake_npu.zeroized = 0U;
        if (!MTFS_TEST_CHECK(test,
                mtfs_sentinel_npu_close(&npu_context, 100U) ==
                    MTFS_ERROR_NOT_READY && npu_context.open && fake_npu.locked &&
                !fake_npu.zeroized && npu_copy[0] == (uint8_t)0xa5U,
                "close failure retains ownership and does not zeroize active memory"))
            return 1;
        fake_npu.fail_close = 0U;
        if (!MTFS_TEST_CHECK(test,
                mtfs_sentinel_npu_close(&npu_context, 100U) == MTFS_OK &&
                !fake_npu.locked && fake_npu.zeroized && npu_copy[0] == 0U &&
                mtfs_sentinel_npu_open(&npu_context, &npu_config, &bundle, 1U,
                    npu_copy, 64U, region_policy, 3U, 100U) == MTFS_OK &&
                mtfs_sentinel_npu_close(&npu_context, 100U) == MTFS_OK,
                "successful retry closes, zeroizes, unlocks, and permits reopen"))
            return 1;
        fake_npu.zeroized = 0U;
        if (!MTFS_TEST_CHECK(test,
                mtfs_sentinel_requantize_q4_to_int8(1, 1U, 3U, 0,
                    &output[0]) == MTFS_OK && output[0] == 1 &&
                mtfs_sentinel_requantize_q4_to_int8(-1, 1U, 3U, 0,
                    &output[0]) == MTFS_OK && output[0] == -1 &&
                mtfs_sentinel_requantize_int8_to_q4(127, UINT32_MAX, 31U,
                    -128, &output[0]) == MTFS_OK && output[0] == 127,
                "requantization fixes ties away from zero and saturates")) return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_runtime_regions_validate_policy(&bundle, 1U,
                region_policy, 3U, &policy_result) == MTFS_OK &&
            policy_result.accepted_mask == 7U && policy_result.owned_mask == 3U &&
            policy_result.zeroize_mask == 3U &&
            policy_result.global_serialization_required == 1U,
            "board policy independently authorizes every NPU region")) return 1;
    region_policy[0].allowed_inference_access = 1U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_runtime_regions_validate_policy(&bundle, 1U,
                region_policy, 3U, &policy_result) == MTFS_ERROR_NOT_SUPPORTED,
            "package execute request cannot expand board policy")) return 1;
    region_policy[0].allowed_inference_access = 5U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_memory_plan(&bundle, &memory_plan) == MTFS_OK &&
            memory_plan.required_alignment == 16U &&
            memory_plan.persistent_offset[0] == 2196U &&
            memory_plan.persistent_offset[1] == 2240U &&
            memory_plan.scratch_offset == 2304U &&
            memory_plan.scratch_size == 48U && memory_plan.required_ram == 2352U,
            "count caller-relative executable but not fixed or contained regions"))
        return 1;
    make_dual_bundle_ordered(reversed, 1);
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(reversed, TEST_DUAL_BUNDLE_SIZE,
                &policy, &bundle) == MTFS_OK && bundle.runtime_count == 2U &&
            bundle.accelerator_id == 0x4e505531U &&
            mtfs_sentinel_bundle_runtime_get(&bundle, 0U, &runtime_info) == MTFS_OK &&
            runtime_info.runtime_type == MTFS_SENTINEL_RUNTIME_NPU &&
            runtime_info.runtime_index == 0U &&
            runtime_info.binary == reversed + 1072U &&
            mtfs_sentinel_bundle_runtime_find(&bundle, 0x4e505250U,
                0x4e505531U, &runtime_info) == MTFS_OK &&
            runtime_info.runtime_index == 0U &&
            mtfs_sentinel_bundle_runtime_get(&bundle, 1U, &runtime_info) == MTFS_OK &&
            runtime_info.runtime_type == MTFS_SENTINEL_RUNTIME_CPU_INT8 &&
            runtime_info.runtime_index == 1U &&
            mtfs_sentinel_bundle_runtime_find(&bundle,
                MTFS_SENTINEL_PROVIDER_CPU_REFERENCE,
                MTFS_SENTINEL_OUTER_ACCELERATOR_CPU, &runtime_info) == MTFS_OK &&
            runtime_info.runtime_index == 1U &&
            mtfs_sentinel_bundle_memory_plan(&bundle, &memory_plan) == MTFS_OK &&
            memory_plan.persistent_offset[0] == 2208U &&
            memory_plan.persistent_size[0] == 64U &&
            memory_plan.persistent_offset[1] == 2272U &&
            memory_plan.persistent_size[1] == 32U &&
            memory_plan.scratch_offset == 2304U &&
            memory_plan.scratch_size == 48U && memory_plan.required_ram == 2352U,
            "bind reversed NPU/CPU identity to the corresponding memory slots"))
        return 1;
    (void)memcpy(dual_damaged, dual, TEST_DUAL_BUNDLE_SIZE);
    put16(dual_damaged + 32U + 3U * 32U, 0x7777U);
    put16(dual_damaged + 32U + 3U * 32U + 2U, 0U);
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(dual_damaged, TEST_DUAL_BUNDLE_SIZE,
                &policy, &bundle) == MTFS_OK && bundle.runtime_count == 1U &&
            mtfs_sentinel_bundle_runtime_get(&bundle, 0U, &runtime_info) == MTFS_OK &&
            runtime_info.runtime_type == MTFS_SENTINEL_RUNTIME_NPU &&
            runtime_info.runtime_index == 0U &&
            mtfs_sentinel_bundle_runtime_find(&bundle, 0x4e505250U,
                0x4e505531U, &runtime_info) == MTFS_OK &&
            runtime_info.runtime_index == 0U &&
            mtfs_sentinel_bundle_memory_plan(&bundle, &memory_plan) == MTFS_OK &&
            memory_plan.persistent_offset[0] == 2208U &&
            memory_plan.persistent_size[0] == 64U &&
            memory_plan.scratch_offset == 0U &&
            memory_plan.scratch_size == 0U && memory_plan.required_ram == 2272U,
            "bundle v1 parser and API support an NPU descriptor v2 runtime")) return 1;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(dual, TEST_DUAL_BUNDLE_SIZE,
                &policy, &bundle) == MTFS_OK &&
            mtfs_sentinel_bundle_memory_plan(&bundle, &memory_plan) == MTFS_OK,
            "restore dual runtime fixture")) return 1;
#if MTFS_ENABLE_SEALED_MODEL
    {
        mtfs_model_info_t outer;
        (void)memset(&outer, 0, sizeof(outer));
        outer.api_version = MTFS_MODEL_INFO_API_VERSION;
        outer.struct_size = (uint32_t)sizeof(outer);
        outer.target_id = 0x52413850U;
        outer.accelerator_id = 0x4e505531U;
        outer.model_format = MTFS_MODEL_FORMAT_SENTINEL_BUNDLE_V1;
        outer.payload_size = TEST_DUAL_BUNDLE_SIZE;
        outer.required_ram = memory_plan.required_ram;
        if (!MTFS_TEST_CHECK(test,
                mtfs_sentinel_bundle_parse_model_payload(dual,
                    TEST_DUAL_BUNDLE_SIZE, &outer, 0x53504920U, 123U,
                    &bundle) == MTFS_OK,
                "sealed adapter accepts descriptor-derived CPU+NPU RAM")) return 1;
        outer.required_ram = memory_plan.required_ram - 1U;
        if (!MTFS_TEST_CHECK(test,
                mtfs_sentinel_bundle_parse_model_payload(dual,
                    TEST_DUAL_BUNDLE_SIZE, &outer, 0x53504920U, 123U,
                    &bundle) == MTFS_ERROR_BUFFER_TOO_SMALL,
                "sealed adapter rejects under-sized CPU+NPU RAM")) return 1;
    }
#endif
    (void)memcpy(dual_damaged, dual, TEST_DUAL_BUNDLE_SIZE);
    put32(dual_damaged + 1792U + 52U, 100U);
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(dual_damaged, TEST_DUAL_BUNDLE_SIZE,
                &policy, &bundle) == MTFS_ERROR_MALFORMED_FORMAT,
            "reject an NPU summary that diverges from its region table"))
        return 1;
    make_bundle(bytes);
    policy.expected_accelerator_id = MTFS_SENTINEL_OUTER_ACCELERATOR_CPU;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(bytes, sizeof(bytes), &policy, &bundle) == MTFS_OK,
            "restore CPU-only fixture after runtime API checks")) return 1;
    if (!MTFS_TEST_CHECK(test, mtfs_sentinel_cpu_init(&cpu, &bundle) == MTFS_OK,
            "initialize fixed topology CPU runtime")) return 1;
    (void)memset(input, 0, sizeof(input));
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_cpu_infer(&cpu, input, work, sizeof(work), output, &result) == MTFS_OK &&
            result.score_q8 == 0U && result.anomaly == 0U,
            "score below threshold is normal")) return 1;
    for (i = 0U; i < 24U; ++i) input[i] = 1;
    (void)memset(output, 0x55, sizeof(output));
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_cpu_infer(&cpu, input, work, sizeof(work), output, &result) == MTFS_OK &&
            result.score_q8 == 1U && result.anomaly == 0U,
            "score equality is normal under strict greater-than comparison")) return 1;
    input[0] = 4;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_cpu_infer(&cpu, input, work, sizeof(work), output, &result) == MTFS_OK &&
            result.score_q8 > 1U && result.anomaly == 1U,
            "score immediately above threshold is anomalous")) return 1;
    (void)memset(input, 0, sizeof(input));
    (void)memset(output, 0, sizeof(output));
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_score_interval_q8(input, output, 1U, 6U,
                &score_interval) == MTFS_OK &&
            score_interval.score_min_q8 == 0U &&
            score_interval.score_max_q8 == 1U &&
            score_interval.decision_class ==
                MTFS_SENTINEL_DECISION_DEFINITELY_NORMAL,
            "bounded Q4 output gives an exact definitely-normal interval"))
        return 1;
    input[0] = 12;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_score_interval_q8(input, output, 1U, 6U,
                &score_interval) == MTFS_OK &&
            score_interval.score_min_q8 == 5U &&
            score_interval.score_max_q8 == 8U &&
            score_interval.decision_class ==
                MTFS_SENTINEL_DECISION_AMBIGUOUS_CPU_ARBITRATION,
            "threshold-crossing interval requires CPU arbitration")) return 1;
    for (i = 0U; i < 24U; ++i) input[i] = 10;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_score_interval_q8(input, output, 1U, 6U,
                &score_interval) == MTFS_OK &&
            score_interval.score_min_q8 == 81U &&
            score_interval.score_max_q8 == 121U &&
            score_interval.decision_class ==
                MTFS_SENTINEL_DECISION_DEFINITELY_ANOMALY,
            "bounded Q4 output gives an exact definitely-anomaly interval"))
        return 1;
    for (i = 0U; i < 24U; ++i) {
        input[i] = -128;
        output[i] = 127;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_score_interval_q8(input, output, 1U, 6U,
                &score_interval) == MTFS_OK &&
            score_interval.score_min_q8 == 64516U &&
            score_interval.score_max_q8 == 65025U &&
            mtfs_sentinel_score_interval_q8(input, output, 256U, 6U,
                &score_interval) == MTFS_ERROR_INVALID_ARGUMENT,
            "score interval clamps int8 candidates and rejects excess limits"))
        return 1;
    for (i = 0U; i < 6U; ++i)
        if (!test_alias_pair(test, &cpu, i)) return 1;
    make_feature(&feature);
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_feature_encode_raw(&feature, raw) == MTFS_OK &&
            raw[2] == 1000U && raw[7] == 0U && raw[21] == 333U,
            "encode all inclusive histogram groups and timing shares")) return 1;
    make_feature(&feature);
    feature.operation[0].timing_samples = 2000U;
    feature.operation[0].latency_histogram[0] = 1999U;
    feature.operation[0].latency_histogram[4] = 1U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_feature_encode_raw(&feature, raw) == MTFS_OK &&
            raw[2] == 1000U && raw[3] == 1U,
            "permille uses exact round-nearest at both half boundaries")) return 1;
    make_feature(&feature);
    feature.operation[0].timing_invalid = UINT64_MAX - 10U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_feature_encode_raw(&feature, raw) == MTFS_OK &&
            raw[1] == 1000U,
            "permille handles a huge numerator and denominator without overflow"))
        return 1;
    make_feature(&feature);
    for (i = 0U; i < 3U; ++i) {
        feature.operation[i].timing_samples = UINT64_MAX / 3U;
        feature.operation[i].latency_histogram[0] = UINT64_MAX / 3U;
        feature.operation[i].average_latency_us = 1U;
        feature.operation[i].total_latency_us = UINT64_MAX / 3U;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_feature_encode_raw(&feature, raw) == MTFS_OK &&
            raw[2] == 1000U && raw[3] == 0U && raw[21] == 333U,
            "permille handles zero, numerator-equals-denominator, and uint64 bounds"))
        return 1;
    make_feature(&feature);
    feature.io_errors = 1U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_feature_encode_raw(&feature, raw) == MTFS_ERROR_NOT_READY,
            "hard fault never enters AI path")) return 1;
    (void)memset(&normalization, 0, sizeof(normalization));
    for (i = 0U; i < 24U; ++i) normalization.inverse_std_q20[i] = 1 << 20U;
    (void)memset(raw, 0, sizeof(raw));
    raw[0] = 8U; raw[1] = 9U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_normalize_int8(&normalization, raw, input) == MTFS_OK &&
            input[0] == 127 && input[1] == 127,
            "positive z clamp saturates to signed int8 maximum")) return 1;
    normalization.mean_q16[2] = INT64_C(8) << 16U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_normalize_int8(&normalization, raw, input) == MTFS_OK &&
            input[2] == -128,
            "negative z clamp represents minus eight exactly")) return 1;
    raw[3] = 1000000U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_normalize_int8(&normalization, raw, input) == MTFS_OK &&
            input[3] == 127,
            "maximum latency raw feature uses checked int64 normalization")) return 1;
    make_bundle(damaged);
    put32(damaged + 1664U, 40000U);
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(damaged, sizeof(damaged), &policy, &bundle) == MTFS_OK &&
            mtfs_sentinel_cpu_init(&cpu, &bundle) == MTFS_OK,
            "accept bounded output-saturation fixture")) return 1;
    (void)memset(input, 0, sizeof(input));
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_cpu_infer(&cpu, input, work, sizeof(work), output, &result) == MTFS_OK &&
            output[0] == 127,
            "linear output saturates without exposing an intermediate")) return 1;
    make_bundle(damaged);
    put32(damaged + 1664U, UINT32_C(0x7fffffff));
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(damaged, sizeof(damaged), &policy, &bundle) ==
                MTFS_ERROR_OVERFLOW,
            "reject model bias that cannot guarantee int32 accumulation")) return 1;
    (void)memcpy(damaged, bytes, sizeof(bytes)); damaged[0] ^= 1U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(damaged, sizeof(damaged), &policy, &bundle) ==
                MTFS_ERROR_MALFORMED_FORMAT,
            "reject bad magic without publishing a bundle")) return 1;
    (void)memcpy(damaged, bytes, sizeof(bytes)); put32(damaged + 32U + 4U, 193U);
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(damaged, sizeof(damaged), &policy, &bundle) ==
                MTFS_ERROR_MALFORMED_FORMAT,
            "reject misaligned overlapping section")) return 1;
    policy.expected_transport_id = 0x49444d41U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(bytes, sizeof(bytes), &policy, &bundle) ==
                MTFS_ERROR_UNSUPPORTED_FORMAT,
            "reject outer and inner identity mismatch")) return 1;
    policy.expected_transport_id = 0x53504920U;
    policy.expected_accelerator_id = 0x4e505531U;
    make_dual_bundle(dual_damaged);
    put32(dual_damaged + 32U + 4U * 32U + 16U,
        MTFS_SENTINEL_PROVIDER_CPU_REFERENCE);
    put32(dual_damaged + 1792U + 4U,
        MTFS_SENTINEL_PROVIDER_CPU_REFERENCE);
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(dual_damaged, TEST_DUAL_BUNDLE_SIZE,
                &policy, &bundle) == MTFS_ERROR_MALFORMED_FORMAT,
            "embedded parser rejects duplicate runtime providers")) return 1;
    make_dual_bundle(dual_damaged); dual_damaged[1792U + 64U] = 1U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(dual_damaged, TEST_DUAL_BUNDLE_SIZE,
                &policy, &bundle) == MTFS_ERROR_UNSUPPORTED_FORMAT,
            "embedded parser compares canonical model hashes across runtimes")) return 1;
    make_dual_bundle(dual_damaged);
    put16(dual_damaged + 32U + 3U * 32U, 0x0101U);
    put16(dual_damaged + 688U + 2U, MTFS_SENTINEL_RUNTIME_NPU);
    put32(dual_damaged + 688U + 8U, 0x4e505531U);
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(dual_damaged, TEST_DUAL_BUNDLE_SIZE,
                &policy, &bundle) == MTFS_ERROR_UNSUPPORTED_FORMAT,
            "V1 embedded parser rejects a second NPU runtime")) return 1;
    make_dual_bundle(dual_damaged); dual_damaged[1792U + 164U] = 1U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(dual_damaged, TEST_DUAL_BUNDLE_SIZE,
                &policy, &bundle) == MTFS_ERROR_MALFORMED_FORMAT,
            "embedded parser rejects runtime descriptor reserved bytes")) return 1;
    make_dual_bundle(dual_damaged); put32(dual_damaged + 1792U + 160U, 193U);
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(dual_damaged, TEST_DUAL_BUNDLE_SIZE,
                &policy, &bundle) == MTFS_ERROR_MALFORMED_FORMAT,
            "embedded parser rejects a misaligned runtime binary offset")) return 1;
    make_dual_bundle(dual_damaged);
    put32(dual_damaged + 1792U + 52U, UINT32_MAX);
    put64(dual_damaged + 1792U + 192U + 48U, UINT32_MAX);
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(dual_damaged, TEST_DUAL_BUNDLE_SIZE,
                &policy, &bundle) == MTFS_ERROR_OVERFLOW,
            "embedded parser rejects a caller-relative RAM plan overflow")) return 1;
    /* Deterministic in-process mutation fuzz smoke test: every byte decoder
     * call must terminate safely and publish only a fully valid result. */
    for (i = 0U; i < 256U; ++i) {
        uint32_t offset = (i * 2654435761U) % TEST_BUNDLE_SIZE;
        make_bundle(damaged);
        damaged[offset] ^= (uint8_t)(1U << (i & 7U));
        policy.expected_accelerator_id = MTFS_SENTINEL_OUTER_ACCELERATOR_CPU;
        (void)mtfs_sentinel_bundle_parse(damaged, sizeof(damaged), &policy, &bundle);
    }
    return 0;
}
