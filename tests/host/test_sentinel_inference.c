#include "test_sentinel_inference.h"

#include <stdint.h>
#include <string.h>

#include "mtfs_sentinel_inference.h"
#if MTFS_ENABLE_SEALED_MODEL
#include "mtfs_sentinel_sealed_adapter.h"
#endif

#define TEST_BUNDLE_SIZE (1762U)
#define TEST_DUAL_BUNDLE_SIZE (2002U)

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8U);
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8U);
    p[2] = (uint8_t)(value >> 16U); p[3] = (uint8_t)(value >> 24U);
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
    put16(runtime, 1U); put16(runtime + 2U, 2U);
    put32(runtime + 4U, 0x4e505250U); put32(runtime + 8U, 0x4e505531U);
    put32(runtime + 12U, 0x564e4431U); put32(runtime + 16U, 1U);
    put32(runtime + 20U, 1U); put16(runtime + 24U, 24U);
    put16(runtime + 26U, 24U); runtime[28] = runtime[29] = 1U;
    put32(runtime + 32U, 1U); put32(runtime + 36U, 4U);
    put32(runtime + 40U, 1U); put32(runtime + 44U, 4U);
    put32(runtime + 48U, 16U); put32(runtime + 52U, 64U);
    put32(runtime + 56U, 128U); put32(runtime + 60U, 16U);
    put32(runtime + 160U, 192U);
    (void)memset(runtime + 192U, 0xa5, 16U);
}

static void make_dual_bundle_ordered(
    uint8_t bundle[TEST_DUAL_BUNDLE_SIZE], int npu_first)
{
    uint8_t cpu_bundle[TEST_BUNDLE_SIZE];
    uint32_t cpu_offset = npu_first ? 896U : 688U;
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
        npu_first ? npu_offset : cpu_offset, npu_first ? 208U : 1104U,
        npu_first ? 16U : 4U,
        npu_first ? 0x4e505250U : 0x43505552U);
    directory(bundle, 4U, npu_first ? 0x0100U : 0x0101U,
        npu_first ? cpu_offset : npu_offset, npu_first ? 1104U : 208U,
        npu_first ? 4U : 16U,
        npu_first ? 0x43505552U : 0x4e505250U);
    directory(bundle, 5U, 4U, 2000U, 2U, 4U, 0U);
    (void)memcpy(bundle + 224U, cpu_bundle + 192U, 128U);
    (void)memcpy(bundle + 352U, cpu_bundle + 320U, 304U);
    (void)memcpy(bundle + 656U, cpu_bundle + 624U, 32U);
    (void)memcpy(bundle + cpu_offset, cpu_bundle + 656U, 1104U);
    (void)memcpy(bundle + 2000U, cpu_bundle + 1760U, 2U);
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
    mtfs_sentinel_bundle_policy_t policy = {1U, (uint16_t)sizeof(policy),
        0x52413850U, 0x53504920U, 0x43505520U, 0x534e5431U, 123U, 4096U};
    mtfs_sentinel_bundle_t bundle;
    mtfs_sentinel_cpu_context_t cpu;
    mtfs_sentinel_inference_result_t result;
    mtfs_sentinel_runtime_info_t runtime_info;
    mtfs_sentinel_memory_plan_t memory_plan;
    mtfs_sentinel_normalization_t normalization;
    mtfs_sentinel_feature_v1_t feature;
    uint32_t raw[24];
    int8_t input[24], output[24];
    uint32_t i, runtime_count;
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
            runtime_info.model_version == 1U && runtime_info.runtime_abi == 1U &&
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
            runtime_info.scratch_memory == 128U &&
            runtime_info.binary == dual + 1984U && runtime_info.binary_size == 16U &&
            runtime_info.binary[0] == 0xa5U &&
            runtime_info.canonical_model_hash[0] == 0U &&
            runtime_info.runtime_binary_hash[0] == 0U &&
            runtime_info.conversion_manifest_hash[0] == 0U &&
            mtfs_sentinel_bundle_runtime_find(&bundle, 0x4e505250U,
                0x4e505531U, &runtime_info) == MTFS_OK &&
            runtime_info.runtime_index == 1U,
            "expose a target NPU runtime without target-side directory parsing")) return 1;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_memory_plan(&bundle, &memory_plan) == MTFS_OK &&
            memory_plan.required_alignment == 16U &&
            memory_plan.persistent_offset[0] == 2004U &&
            memory_plan.persistent_offset[1] == 2048U &&
            memory_plan.scratch_offset == 2112U &&
            memory_plan.scratch_size == 128U && memory_plan.required_ram == 2240U,
            "sum persistent memory and share maximum scratch for sequential comparison"))
        return 1;
    make_dual_bundle_ordered(reversed, 1);
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(reversed, TEST_DUAL_BUNDLE_SIZE,
                &policy, &bundle) == MTFS_OK && bundle.runtime_count == 2U &&
            bundle.accelerator_id == 0x4e505531U &&
            mtfs_sentinel_bundle_runtime_get(&bundle, 0U, &runtime_info) == MTFS_OK &&
            runtime_info.runtime_type == MTFS_SENTINEL_RUNTIME_NPU &&
            runtime_info.runtime_index == 0U &&
            runtime_info.binary == reversed + 880U &&
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
            memory_plan.persistent_offset[0] == 2016U &&
            memory_plan.persistent_size[0] == 64U &&
            memory_plan.persistent_offset[1] == 2080U &&
            memory_plan.persistent_size[1] == 32U &&
            memory_plan.scratch_offset == 2112U &&
            memory_plan.scratch_size == 128U && memory_plan.required_ram == 2240U,
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
            memory_plan.persistent_offset[0] == 2016U &&
            memory_plan.persistent_size[0] == 64U &&
            memory_plan.scratch_offset == 2080U &&
            memory_plan.scratch_size == 128U && memory_plan.required_ram == 2208U,
            "V1 parser and API support an NPU-only runtime")) return 1;
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
    put32(dual_damaged + 32U + 4U * 32U + 12U, 32U);
    put32(dual_damaged + 1792U + 48U, 32U);
    put32(dual_damaged + 1792U + 52U, 100U);
    put32(dual_damaged + 1792U + 56U, 200U);
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(dual_damaged, TEST_DUAL_BUNDLE_SIZE,
                &policy, &bundle) == MTFS_OK &&
            mtfs_sentinel_bundle_memory_plan(&bundle, &memory_plan) == MTFS_OK &&
            memory_plan.required_alignment == 32U &&
            memory_plan.persistent_offset[1] == 2048U &&
            memory_plan.scratch_offset == 2176U &&
            memory_plan.required_ram == 2376U,
            "RAM plan follows changed NPU alignment, persistent, and scratch values"))
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
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_bundle_parse(dual_damaged, TEST_DUAL_BUNDLE_SIZE,
                &policy, &bundle) == MTFS_ERROR_OVERFLOW,
            "embedded parser rejects a V1 RAM plan overflow")) return 1;
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
