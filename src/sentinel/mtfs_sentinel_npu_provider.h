/* Target-neutral, heapless Storage Sentinel NPU provider boundary. */
#ifndef MTFS_SENTINEL_NPU_PROVIDER_H
#define MTFS_SENTINEL_NPU_PROVIDER_H

#include "mtfs_sentinel_inference.h"

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_SENTINEL_NPU_PROVIDER_API_VERSION (UINT16_C(1))
#define MTFS_SENTINEL_NPU_MAX_ACTUAL_REGIONS (UINT32_C(8))
#define MTFS_SENTINEL_NPU_MAX_FIXED_ZEROIZE_REGIONS (UINT32_C(8))

typedef struct mtfs_sentinel_npu_actual_region
{
    uint16_t kind;
    uint16_t placement;
    uint32_t alignment;
    uint64_t logical_size;
    uint64_t storage_size;
    uint64_t address_or_offset;
} mtfs_sentinel_npu_actual_region_t;

typedef struct mtfs_sentinel_npu_actual_info
{
    uint32_t runtime_abi;
    uint32_t runtime_variant;
    uint32_t runtime_extra;
    uint32_t copy_size;
    uint32_t copy_alignment;
    uint32_t parameters_offset;
    uint32_t parameters_logical_size;
    uint32_t parameters_storage_size;
    uint32_t activation_address;
    uint32_t activation_size;
    uint32_t external_ram_size;
    uint32_t region_count;
    mtfs_sentinel_npu_actual_region_t regions[MTFS_SENTINEL_NPU_MAX_ACTUAL_REGIONS];
    uint8_t runtime_binary_hash[32];
} mtfs_sentinel_npu_actual_info_t;

typedef mtfs_error_t (*mtfs_sentinel_npu_inspect_fn)(void *target,
    const uint8_t *binary, uint32_t binary_size,
    mtfs_sentinel_npu_actual_info_t *actual);
typedef mtfs_error_t (*mtfs_sentinel_npu_install_fn)(void *target,
    const uint8_t *binary, uint32_t binary_size, void *copy_memory,
    uint32_t copy_size, const mtfs_sentinel_runtime_info_t *runtime);
typedef mtfs_error_t (*mtfs_sentinel_npu_infer_fn)(void *target,
    const int8_t input[MTFS_SENTINEL_FEATURE_DIMENSION],
    int8_t output[MTFS_SENTINEL_FEATURE_DIMENSION], uint32_t timeout_ms);
typedef mtfs_error_t (*mtfs_sentinel_npu_close_fn)(void *target);
typedef mtfs_error_t (*mtfs_sentinel_npu_lock_fn)(void *target,
    uint32_t timeout_ms);
typedef void (*mtfs_sentinel_npu_unlock_fn)(void *target);
typedef void (*mtfs_sentinel_npu_zeroize_fn)(void *target, void *address,
    uint32_t size);
typedef uint32_t (*mtfs_sentinel_npu_cycle_count_fn)(void *context);

/* Diagnostic timing is opt-in and does not alter the normal inference path. */
typedef struct mtfs_sentinel_npu_inference_profile
{
    uint64_t input_requantize_cycles;
    uint64_t target_infer_cycles;
    uint64_t output_requantize_cycles;
    uint64_t score_decision_cycles;
    uint64_t total_cycles;
    uint32_t attempted;
    uint32_t completed;
} mtfs_sentinel_npu_inference_profile_t;

typedef struct mtfs_sentinel_npu_provider_ops
{
    mtfs_sentinel_npu_inspect_fn inspect;
    mtfs_sentinel_npu_install_fn install;
    mtfs_sentinel_npu_infer_fn infer;
    mtfs_sentinel_npu_close_fn close;
    mtfs_sentinel_npu_lock_fn lock;
    mtfs_sentinel_npu_unlock_fn unlock;
    mtfs_sentinel_npu_zeroize_fn zeroize;
} mtfs_sentinel_npu_provider_ops_t;

typedef struct mtfs_sentinel_npu_provider_config
{
    uint16_t api_version;
    uint16_t struct_size;
    uint32_t provider_id;
    uint32_t accelerator_id;
    const mtfs_sentinel_npu_provider_ops_t *ops;
    void *target;
} mtfs_sentinel_npu_provider_config_t;

typedef struct mtfs_sentinel_npu_context
{
    uint16_t api_version;
    uint16_t struct_size;
    mtfs_sentinel_npu_provider_config_t config;
    mtfs_sentinel_runtime_info_t runtime;
    mtfs_sentinel_runtime_policy_result_t policy;
    void *copy_memory;
    uint32_t copy_size;
    void *fixed_zeroize_memory[MTFS_SENTINEL_NPU_MAX_FIXED_ZEROIZE_REGIONS];
    uint32_t fixed_zeroize_size[MTFS_SENTINEL_NPU_MAX_FIXED_ZEROIZE_REGIONS];
    uint32_t fixed_zeroize_count;
    uint64_t threshold_q8;
    uint8_t open;
    uint8_t locked;
    uint8_t reserved[2];
} mtfs_sentinel_npu_context_t;

mtfs_error_t mtfs_sentinel_npu_open(mtfs_sentinel_npu_context_t *context,
    const mtfs_sentinel_npu_provider_config_t *config,
    const mtfs_sentinel_bundle_t *bundle, uint32_t runtime_index,
    void *copy_memory, uint32_t copy_size,
    const mtfs_sentinel_runtime_region_policy_t *policies,
    uint32_t policy_count, uint32_t timeout_ms);
mtfs_error_t mtfs_sentinel_npu_infer(mtfs_sentinel_npu_context_t *context,
    const int8_t input_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
    int8_t output_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
    uint32_t timeout_ms, mtfs_sentinel_inference_result_t *result);
mtfs_error_t mtfs_sentinel_npu_infer_detailed(
    mtfs_sentinel_npu_context_t *context,
    const int8_t input_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
    int8_t raw_output_int8[MTFS_SENTINEL_FEATURE_DIMENSION],
    int8_t output_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
    uint32_t timeout_ms, mtfs_sentinel_inference_result_t *result);
mtfs_error_t mtfs_sentinel_npu_infer_profiled_detailed(
    mtfs_sentinel_npu_context_t *context,
    const int8_t input_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
    int8_t raw_output_int8[MTFS_SENTINEL_FEATURE_DIMENSION],
    int8_t output_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
    uint32_t timeout_ms, mtfs_sentinel_inference_result_t *result,
    mtfs_sentinel_npu_cycle_count_fn cycle_count, void *cycle_context,
    mtfs_sentinel_npu_inference_profile_t *profile);
mtfs_error_t mtfs_sentinel_npu_close(mtfs_sentinel_npu_context_t *context,
    uint32_t timeout_ms);
mtfs_error_t mtfs_sentinel_requantize_q4_to_int8(int8_t input_q4,
    uint32_t scale_numerator, uint32_t scale_shift, int8_t zero_point,
    int8_t *output);
mtfs_error_t mtfs_sentinel_requantize_int8_to_q4(int8_t input,
    uint32_t scale_numerator, uint32_t scale_shift, int8_t zero_point,
    int8_t *output_q4);
#ifdef __cplusplus
}
#endif
#endif
#endif
