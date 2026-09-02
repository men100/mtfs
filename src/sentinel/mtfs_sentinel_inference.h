/* Storage Sentinel bundle v1 and heapless fixed-point CPU reference API. */
#ifndef MTFS_SENTINEL_INFERENCE_H
#define MTFS_SENTINEL_INFERENCE_H

#include "../mtfs_config.h"

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE

#include <stddef.h>
#include <stdint.h>
#include "../mtfs_error.h"
#include "mtfs_sentinel.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_SENTINEL_INFERENCE_API_VERSION (UINT16_C(1))
#define MTFS_SENTINEL_BUNDLE_VERSION (UINT16_C(1))
#define MTFS_SENTINEL_BUNDLE_HEADER_SIZE (32U)
#define MTFS_SENTINEL_BUNDLE_DIRECTORY_ENTRY_SIZE (32U)
#define MTFS_SENTINEL_BUNDLE_MAX_SIZE (UINT32_C(1048576))
#define MTFS_SENTINEL_BUNDLE_MAX_SECTIONS (16U)
#define MTFS_SENTINEL_BUNDLE_MAX_RUNTIMES (2U)
#define MTFS_SENTINEL_RUNTIME_DESCRIPTOR_SIZE (192U)
#define MTFS_SENTINEL_MAX_REQUIRED_RAM (UINT32_MAX)
#define MTFS_SENTINEL_FEATURE_DIMENSION (24U)
#define MTFS_SENTINEL_CPU_LAYER_COUNT (4U)
#define MTFS_SENTINEL_CPU_MODEL_BINARY_HEADER_SIZE (32U)
#define MTFS_SENTINEL_CPU_WEIGHT_COUNT (672U)
#define MTFS_SENTINEL_CPU_BIAS_COUNT (52U)
#define MTFS_SENTINEL_CPU_MODEL_BINARY_SIZE (912U)
#define MTFS_SENTINEL_CPU_WORK_SIZE (48U)
#define MTFS_SENTINEL_CPU_WORK_ALIGNMENT (1U)
#define MTFS_SENTINEL_CPU_PERSISTENT_SIZE_32 (32U)

#define MTFS_SENTINEL_SECTION_FLAG_REQUIRED (UINT16_C(1))
#define MTFS_SENTINEL_SECTION_COMPATIBILITY (UINT16_C(1))
#define MTFS_SENTINEL_SECTION_NORMALIZATION (UINT16_C(2))
#define MTFS_SENTINEL_SECTION_DECISION (UINT16_C(3))
#define MTFS_SENTINEL_SECTION_PROVENANCE (UINT16_C(4))
#define MTFS_SENTINEL_SECTION_CPU_INT8_RUNTIME (UINT16_C(0x0100))
#define MTFS_SENTINEL_SECTION_NPU_RUNTIME (UINT16_C(0x0101))

#define MTFS_SENTINEL_RUNTIME_CPU_INT8 (UINT16_C(1))
#define MTFS_SENTINEL_RUNTIME_NPU (UINT16_C(2))
#define MTFS_SENTINEL_DATA_TYPE_INT8 (UINT8_C(1))
#define MTFS_SENTINEL_PROVIDER_CPU_REFERENCE (UINT32_C(0x43505552))
#define MTFS_SENTINEL_MODEL_FORMAT_CPU_INT8_V1 (UINT32_C(0x51414531))
#define MTFS_SENTINEL_TOPOLOGY_24_12_4_12_24 (UINT32_C(0x180c040c))
#define MTFS_SENTINEL_OUTER_MODEL_FORMAT_V1 (UINT32_C(0x534e5431))
#define MTFS_SENTINEL_OUTER_ACCELERATOR_CPU (UINT32_C(0x43505520))

#define MTFS_SENTINEL_TARGET_EK_RA8P1 (UINT32_C(0x52413850))
#define MTFS_SENTINEL_TARGET_STM32N6570_DK (UINT32_C(0x53544e36))
#define MTFS_SENTINEL_TRANSPORT_SPI (UINT32_C(0x53504920))
#define MTFS_SENTINEL_TRANSPORT_SDMMC_IDMA (UINT32_C(0x49444d41))

typedef struct mtfs_sentinel_bundle_policy
{
    uint16_t api_version;
    uint16_t struct_size;
    uint32_t expected_target_id;
    uint32_t expected_transport_id;
    uint32_t expected_accelerator_id;
    uint32_t expected_model_format;
    uint32_t expected_profile_id;
    uint32_t maximum_bundle_size;
} mtfs_sentinel_bundle_policy_t;

typedef struct mtfs_sentinel_normalization
{
    int64_t mean_q16[MTFS_SENTINEL_FEATURE_DIMENSION];
    int32_t inverse_std_q20[MTFS_SENTINEL_FEATURE_DIMENSION];
} mtfs_sentinel_normalization_t;

typedef struct mtfs_sentinel_bundle
{
    uint16_t api_version;
    uint16_t struct_size;
    const uint8_t *bytes;
    uint32_t size;
    uint32_t target_id;
    uint32_t transport_id;
    uint32_t accelerator_id;
    uint32_t model_format;
    uint32_t profile_id;
    uint32_t runtime_count;
    uint64_t threshold_q8;
    mtfs_sentinel_normalization_t normalization;
    const uint8_t *cpu_runtime;
    uint32_t cpu_runtime_size;
    /* Opaque metadata. Embedded code does not parse JSON or recompute SHA-256. */
    const uint8_t *provenance;
    uint32_t provenance_size;
} mtfs_sentinel_bundle_t;

typedef struct mtfs_sentinel_cpu_context
{
    uint16_t api_version;
    uint16_t struct_size;
    const uint8_t *weights;
    const uint8_t *biases;
    uint32_t weights_size;
    uint32_t biases_size;
    uint64_t threshold_q8;
} mtfs_sentinel_cpu_context_t;

typedef struct mtfs_sentinel_inference_result
{
    uint16_t api_version;
    uint16_t struct_size;
    uint64_t score_q8;
    uint64_t threshold_q8;
    uint8_t anomaly;
    uint8_t reserved[7];
} mtfs_sentinel_inference_result_t;

typedef struct mtfs_sentinel_runtime_info
{
    uint16_t api_version;
    uint16_t struct_size;
    uint16_t runtime_type;
    /* Zero-origin slot in persistent_offset[]/persistent_size[]. */
    uint16_t runtime_index;
    uint32_t provider_id;
    uint32_t accelerator_id;
    uint32_t model_format;
    uint32_t model_version;
    uint32_t runtime_abi;
    uint16_t input_dimension;
    uint16_t output_dimension;
    uint8_t input_data_type;
    uint8_t output_data_type;
    int8_t input_zero_point;
    int8_t output_zero_point;
    uint32_t input_scale_numerator;
    uint32_t input_scale_shift;
    uint32_t output_scale_numerator;
    uint32_t output_scale_shift;
    uint32_t required_alignment;
    uint32_t persistent_memory;
    uint32_t scratch_memory;
    uint32_t binary_size;
    const uint8_t *binary;
    uint8_t canonical_model_hash[32];
    uint8_t runtime_binary_hash[32];
    uint8_t conversion_manifest_hash[32];
} mtfs_sentinel_runtime_info_t;

/*
 * Outer required_ram covers this plan only. The parsed bundle/API structs,
 * raw features, tensors, and inference result remain caller-owned elsewhere.
 * The bundle occupies [0, bundle_size). Persistent regions are retained for
 * every runtime, while scratch is shared because CPU/NPU comparison is
 * sequential. persistent_offset[runtime_index] and persistent_size[
 * runtime_index] describe the region for that runtime. The allocation base
 * must satisfy required_alignment and the allocation must contain at least
 * required_ram bytes. The parser/planner validates additions and offsets;
 * callers must bounds-check allocation_size against required_ram before forming
 * allocation_base + persistent_offset[] or allocation_base + scratch_offset.
 * Integrity of a plain bundle is not established until an outer sealed AEAD
 * authenticates it; embedded provenance remains opaque after authentication.
 */
typedef struct mtfs_sentinel_memory_plan
{
    uint16_t api_version;
    uint16_t struct_size;
    uint32_t runtime_count;
    uint32_t required_alignment;
    uint32_t scratch_size;
    uint64_t bundle_size;
    uint64_t required_ram;
    uint64_t scratch_offset;
    uint64_t persistent_offset[MTFS_SENTINEL_BUNDLE_MAX_RUNTIMES];
    uint32_t persistent_size[MTFS_SENTINEL_BUNDLE_MAX_RUNTIMES];
} mtfs_sentinel_memory_plan_t;

/*
 * Lifetime contract: parsing does not copy the bundle. mtfs_sentinel_bundle_t,
 * mtfs_sentinel_runtime_info_t::binary, and an initialized CPU context contain
 * pointers into bytes. The backing buffer must remain alive and byte-for-byte
 * unchanged until all of those views and contexts are no longer used.
 * Runtime binary pointers are returned only after checked section-offset and
 * binary-offset validation.
 */
mtfs_error_t mtfs_sentinel_bundle_parse(const void *bytes, size_t size,
    const mtfs_sentinel_bundle_policy_t *policy,
    mtfs_sentinel_bundle_t *bundle);
mtfs_error_t mtfs_sentinel_bundle_runtime_count(
    const mtfs_sentinel_bundle_t *bundle, uint32_t *runtime_count);
mtfs_error_t mtfs_sentinel_bundle_runtime_get(
    const mtfs_sentinel_bundle_t *bundle, uint32_t index,
    mtfs_sentinel_runtime_info_t *runtime);
/*
 * Provider and accelerator IDs are both mandatory; zero is never a wildcard.
 * Both get and find return the runtime's memory-plan slot in runtime_index.
 */
mtfs_error_t mtfs_sentinel_bundle_runtime_find(
    const mtfs_sentinel_bundle_t *bundle, uint32_t provider_id,
    uint32_t accelerator_id, mtfs_sentinel_runtime_info_t *runtime);
mtfs_error_t mtfs_sentinel_bundle_memory_plan(
    const mtfs_sentinel_bundle_t *bundle,
    mtfs_sentinel_memory_plan_t *plan);
mtfs_error_t mtfs_sentinel_feature_encode_raw(
    const mtfs_sentinel_feature_v1_t *feature,
    uint32_t output[MTFS_SENTINEL_FEATURE_DIMENSION]);
mtfs_error_t mtfs_sentinel_normalize_int8(
    const mtfs_sentinel_normalization_t *normalization,
    const uint32_t raw[MTFS_SENTINEL_FEATURE_DIMENSION],
    int8_t output[MTFS_SENTINEL_FEATURE_DIMENSION]);
mtfs_error_t mtfs_sentinel_cpu_init(mtfs_sentinel_cpu_context_t *context,
    const mtfs_sentinel_bundle_t *bundle);
/* input, output, work, and result must be pairwise non-overlapping. */
mtfs_error_t mtfs_sentinel_cpu_infer(
    const mtfs_sentinel_cpu_context_t *context,
    const int8_t input[MTFS_SENTINEL_FEATURE_DIMENSION],
    void *work, size_t work_size,
    int8_t output[MTFS_SENTINEL_FEATURE_DIMENSION],
    mtfs_sentinel_inference_result_t *result);

#ifdef __cplusplus
}
#endif
#endif /* MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE */
#endif /* MTFS_SENTINEL_INFERENCE_H */
