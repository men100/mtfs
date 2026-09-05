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

#define MTFS_SENTINEL_INFERENCE_API_VERSION (UINT16_C(2))
#define MTFS_SENTINEL_BUNDLE_VERSION (UINT16_C(1))
#define MTFS_SENTINEL_BUNDLE_HEADER_SIZE (32U)
#define MTFS_SENTINEL_BUNDLE_DIRECTORY_ENTRY_SIZE (32U)
#define MTFS_SENTINEL_BUNDLE_MAX_SIZE (UINT32_C(1048576))
#define MTFS_SENTINEL_BUNDLE_MAX_SECTIONS (16U)
#define MTFS_SENTINEL_BUNDLE_MAX_RUNTIMES (2U)
#define MTFS_SENTINEL_RUNTIME_DESCRIPTOR_SIZE (192U)
#define MTFS_SENTINEL_RUNTIME_REGION_ENTRY_SIZE (64U)
#define MTFS_SENTINEL_RUNTIME_REGION_MAX_COUNT (16U)
#define MTFS_SENTINEL_MAX_REQUIRED_RAM (UINT32_MAX)
#define MTFS_SENTINEL_FEATURE_DIMENSION (24U)
#define MTFS_SENTINEL_CPU_LAYER_COUNT (4U)
#define MTFS_SENTINEL_CPU_MODEL_BINARY_HEADER_SIZE (32U)
#define MTFS_SENTINEL_CPU_WEIGHT_COUNT (672U)
#define MTFS_SENTINEL_CPU_BIAS_COUNT (52U)
#define MTFS_SENTINEL_CPU_MODEL_BINARY_SIZE (912U)
#define MTFS_SENTINEL_CPU_TFLITE_INT8_HEADER_SIZE (96U)
#define MTFS_SENTINEL_CPU_TFLITE_LAYER_DESCRIPTOR_SIZE (48U)
#define MTFS_SENTINEL_CPU_TFLITE_INT8_MAX_BINARY_SIZE (4096U)
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
#define MTFS_SENTINEL_RUNTIME_DESCRIPTOR_CPU_VERSION (UINT16_C(1))
#define MTFS_SENTINEL_RUNTIME_DESCRIPTOR_NPU_VERSION (UINT16_C(2))
#define MTFS_SENTINEL_RUNTIME_REGION_FLAG_REQUIRED (UINT16_C(1))
#define MTFS_SENTINEL_REGION_EXECUTABLE_COPY (UINT16_C(1))
#define MTFS_SENTINEL_REGION_ACTIVATION (UINT16_C(2))
#define MTFS_SENTINEL_REGION_PARAMETERS (UINT16_C(3))
#define MTFS_SENTINEL_REGION_EXTERNAL_RW (UINT16_C(4))
#define MTFS_SENTINEL_REGION_PROVIDER_CONTEXT (UINT16_C(5))
#define MTFS_SENTINEL_REGION_RUNTIME_BINARY (UINT16_C(6))
#define MTFS_SENTINEL_REGION_INTERPRETER (UINT16_C(7))
#define MTFS_SENTINEL_REGION_RESOLVER (UINT16_C(8))
#define MTFS_SENTINEL_REGION_TENSOR_ARENA (UINT16_C(9))
#define MTFS_SENTINEL_REGION_ACCELERATOR_CONTEXT (UINT16_C(10))
#define MTFS_SENTINEL_REGION_SEMAPHORE_POOL (UINT16_C(11))
#define MTFS_SENTINEL_REGION_PROVIDER_SYNC (UINT16_C(12))
#define MTFS_SENTINEL_PLACEMENT_CALLER_RELATIVE (UINT16_C(1))
#define MTFS_SENTINEL_PLACEMENT_FIXED_ABSOLUTE (UINT16_C(2))
#define MTFS_SENTINEL_PLACEMENT_BINARY_CONTAINED (UINT16_C(3))
#define MTFS_SENTINEL_PLACEMENT_PROVIDER_ASSIGNED (UINT16_C(4))
#define MTFS_SENTINEL_REGION_ACCESS_READ (UINT32_C(1))
#define MTFS_SENTINEL_REGION_ACCESS_WRITE (UINT32_C(2))
#define MTFS_SENTINEL_REGION_ACCESS_EXECUTE (UINT32_C(4))
#define MTFS_SENTINEL_REGION_LIFETIME_INSTALL (UINT32_C(1))
#define MTFS_SENTINEL_REGION_LIFETIME_INSTANCE (UINT32_C(2))
#define MTFS_SENTINEL_REGION_LIFETIME_INFERENCE (UINT32_C(3))
#define MTFS_SENTINEL_REGION_LIFETIME_PROCESS (UINT32_C(4))
#define MTFS_SENTINEL_REGION_REQUIRE_ZEROIZE (UINT32_C(1))
#define MTFS_SENTINEL_REGION_REQUIRE_CACHE_COHERENCY (UINT32_C(2))
#define MTFS_SENTINEL_REGION_REQUIRE_EXCLUSIVE (UINT32_C(4))
#define MTFS_SENTINEL_REGION_REQUIRE_SHAREABLE (UINT32_C(8))
#define MTFS_SENTINEL_REGION_REQUIRE_INPUT_OUTPUT_SHARED (UINT32_C(16))
#define MTFS_SENTINEL_REGION_POLICY_OWNED (UINT32_C(1))
#define MTFS_SENTINEL_REGION_POLICY_EXCLUSIVE (UINT32_C(2))
#define MTFS_SENTINEL_REGION_POLICY_CACHE_MAINTENANCE (UINT32_C(4))
#define MTFS_SENTINEL_REGION_POLICY_ALLOW_ZEROIZE (UINT32_C(8))
#define MTFS_SENTINEL_REGION_POLICY_GLOBAL_SERIALIZATION (UINT32_C(16))
#define MTFS_SENTINEL_DATA_TYPE_INT8 (UINT8_C(1))
#define MTFS_SENTINEL_PROVIDER_CPU_REFERENCE (UINT32_C(0x43505552))
#define MTFS_SENTINEL_MODEL_FORMAT_CPU_INT8_V1 (UINT32_C(0x51414531))
#define MTFS_SENTINEL_MODEL_FORMAT_CPU_TFLITE_INT8_V2 (UINT32_C(0x54493832))
#define MTFS_SENTINEL_PROVIDER_ST_NEURAL_ART_RELOC (UINT32_C(0x53544e52))
#define MTFS_SENTINEL_ACCELERATOR_NEURAL_ART (UINT32_C(0x4e415254))
#define MTFS_SENTINEL_MODEL_FORMAT_ST_RELOC (UINT32_C(0x5354524c))
#define MTFS_SENTINEL_PROVIDER_RA_TFLM_ETHOSU (UINT32_C(0x52415446))
#define MTFS_SENTINEL_ACCELERATOR_ETHOS_U55 (UINT32_C(0x45553535))
#define MTFS_SENTINEL_MODEL_FORMAT_RA_TFLM_ETHOSU (UINT32_C(0x45555446))
#define MTFS_SENTINEL_TOPOLOGY_24_12_4_12_24 (UINT32_C(0x180c040c))
#define MTFS_SENTINEL_OUTER_MODEL_FORMAT_V1 (UINT32_C(0x534e5431))
#define MTFS_SENTINEL_OUTER_ACCELERATOR_CPU (UINT32_C(0x43505520))

#define MTFS_SENTINEL_DECISION_DEFINITELY_NORMAL (UINT8_C(0))
#define MTFS_SENTINEL_DECISION_DEFINITELY_ANOMALY (UINT8_C(1))
#define MTFS_SENTINEL_DECISION_AMBIGUOUS_CPU_ARBITRATION (UINT8_C(2))

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
    const uint8_t *runtime;
    uint32_t runtime_size;
    uint32_t model_format;
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

typedef struct mtfs_sentinel_score_interval
{
    uint16_t api_version;
    uint16_t struct_size;
    uint64_t score_min_q8;
    uint64_t score_max_q8;
    uint64_t threshold_q8;
    uint8_t decision_class;
    uint8_t reserved[7];
} mtfs_sentinel_score_interval_t;

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
    uint16_t descriptor_version;
    uint16_t region_count;
    uint16_t runtime_version_major;
    uint16_t runtime_version_minor;
    uint32_t runtime_variant;
    uint32_t runtime_extra;
} mtfs_sentinel_runtime_info_t;

typedef struct mtfs_sentinel_runtime_region_info
{
    uint16_t api_version;
    uint16_t struct_size;
    uint16_t kind;
    uint16_t placement;
    uint16_t flags;
    uint16_t reserved;
    uint32_t region_index;
    uint64_t logical_size;
    uint64_t storage_size;
    uint32_t alignment;
    uint32_t provider_pool_id;
    uint64_t address_or_offset;
    uint32_t lifetime;
    uint32_t install_access;
    uint32_t inference_access;
    uint32_t requirements;
} mtfs_sentinel_runtime_region_info_t;

typedef struct mtfs_sentinel_runtime_region_policy
{
    uint32_t provider_id;
    uint32_t accelerator_id;
    uint16_t kind;
    uint16_t placement;
    uint32_t minimum_alignment;
    uint64_t address_minimum;
    uint64_t address_limit;
    uint64_t maximum_storage_size;
    uint32_t allowed_install_access;
    uint32_t allowed_inference_access;
    uint32_t allowed_requirements;
    uint32_t policy_flags;
} mtfs_sentinel_runtime_region_policy_t;

typedef struct mtfs_sentinel_runtime_policy_result
{
    uint16_t api_version;
    uint16_t struct_size;
    uint32_t region_count;
    uint32_t accepted_mask;
    uint32_t owned_mask;
    uint32_t cache_maintenance_mask;
    uint32_t zeroize_mask;
    uint8_t global_serialization_required;
    uint8_t reserved[7];
} mtfs_sentinel_runtime_policy_result_t;

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
mtfs_error_t mtfs_sentinel_bundle_runtime_region_get(
    const mtfs_sentinel_bundle_t *bundle, uint32_t runtime_index,
    uint32_t region_index, mtfs_sentinel_runtime_region_info_t *region);
mtfs_error_t mtfs_sentinel_bundle_runtime_regions_validate_policy(
    const mtfs_sentinel_bundle_t *bundle, uint32_t runtime_index,
    const mtfs_sentinel_runtime_region_policy_t *policies,
    uint32_t policy_count, mtfs_sentinel_runtime_policy_result_t *result);
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
mtfs_error_t mtfs_sentinel_score_q8(
    const int8_t input_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
    const int8_t output_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
    uint64_t threshold_q8, mtfs_sentinel_inference_result_t *result);
/*
 * Derives the complete score interval for any output whose every common-Q4
 * element is within maximum_q4_error of reference_output_q4. Candidate values
 * are clamped to int8 before the exact (sum + 12) / 24 rounding is applied.
 */
mtfs_error_t mtfs_sentinel_score_interval_q8(
    const int8_t input_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
    const int8_t reference_output_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
    uint32_t maximum_q4_error, uint64_t threshold_q8,
    mtfs_sentinel_score_interval_t *interval);
mtfs_error_t mtfs_sentinel_cpu_init(mtfs_sentinel_cpu_context_t *context,
    const mtfs_sentinel_bundle_t *bundle);
/* Executes the canonical TFLite int8 domain without common-Q4 conversion. */
mtfs_error_t mtfs_sentinel_cpu_infer_canonical_int8(
    const mtfs_sentinel_cpu_context_t *context,
    const int8_t input[MTFS_SENTINEL_FEATURE_DIMENSION],
    void *work, size_t work_size,
    int8_t output[MTFS_SENTINEL_FEATURE_DIMENSION]);
/* input, output, work, and result must be pairwise non-overlapping. */
mtfs_error_t mtfs_sentinel_cpu_infer(
    const mtfs_sentinel_cpu_context_t *context,
    const int8_t input[MTFS_SENTINEL_FEATURE_DIMENSION],
    void *work, size_t work_size,
    int8_t output[MTFS_SENTINEL_FEATURE_DIMENSION],
    mtfs_sentinel_inference_result_t *result);
mtfs_error_t mtfs_sentinel_cpu_infer_detailed(
    const mtfs_sentinel_cpu_context_t *context,
    const int8_t input[MTFS_SENTINEL_FEATURE_DIMENSION],
    void *work, size_t work_size,
    int8_t raw_output_int8[MTFS_SENTINEL_FEATURE_DIMENSION],
    int8_t output_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
    mtfs_sentinel_inference_result_t *result);

#ifdef __cplusplus
}
#endif
#endif /* MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE */
#endif /* MTFS_SENTINEL_INFERENCE_H */
