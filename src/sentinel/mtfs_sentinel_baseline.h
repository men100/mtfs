/* Heapless per-session baseline and relative preprocessing primitives. */
#ifndef MTFS_SENTINEL_BASELINE_H
#define MTFS_SENTINEL_BASELINE_H

#include "../mtfs_config.h"

#if MTFS_ENABLE_STORAGE_SENTINEL

#include <stdint.h>
#include "../mtfs_error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_SENTINEL_BASELINE_API_VERSION (UINT16_C(1))
#define MTFS_SENTINEL_BASELINE_PREPROCESSING_VERSION (UINT16_C(2))
#define MTFS_SENTINEL_BASELINE_FEATURE_COUNT (24U)
#define MTFS_SENTINEL_BASELINE_MAX_WARMUP_WINDOWS (32U)
#define MTFS_SENTINEL_BASELINE_LATENCY_FEATURE_MASK \
    ((UINT32_C(1) << 0) | (UINT32_C(1) << 7) | (UINT32_C(1) << 14))

typedef enum mtfs_sentinel_baseline_state
{
    MTFS_SENTINEL_BASELINE_UNINITIALIZED = 0,
    MTFS_SENTINEL_BASELINE_WARMUP = 1,
    MTFS_SENTINEL_BASELINE_READY = 2,
    MTFS_SENTINEL_BASELINE_INVALID_DISCONTINUOUS = 3
} mtfs_sentinel_baseline_state_t;

typedef struct mtfs_sentinel_baseline_policy
{
    uint16_t api_version;
    uint16_t struct_size;
    uint16_t preprocessing_version;
    uint16_t warmup_windows;
    uint32_t active_feature_mask;
    uint32_t latency_baseline_floor[3];
    uint32_t relative_scale_floor[MTFS_SENTINEL_BASELINE_FEATURE_COUNT];
    uint8_t maximum_saturated_features;
    uint8_t reserved[3];
} mtfs_sentinel_baseline_policy_t;

typedef struct mtfs_sentinel_baseline_result
{
    int8_t input_q4[MTFS_SENTINEL_BASELINE_FEATURE_COUNT];
    uint32_t saturation_mask;
    uint8_t saturation_count;
    uint8_t out_of_distribution;
    uint8_t reserved[2];
} mtfs_sentinel_baseline_result_t;

typedef struct mtfs_sentinel_baseline
{
    mtfs_sentinel_baseline_policy_t policy;
    uint32_t samples[MTFS_SENTINEL_BASELINE_MAX_WARMUP_WINDOWS]
        [MTFS_SENTINEL_BASELINE_FEATURE_COUNT];
    uint32_t baseline[MTFS_SENTINEL_BASELINE_FEATURE_COUNT];
    uint32_t media_generation;
    uint16_t sample_count;
    uint8_t state;
    uint8_t reserved;
} mtfs_sentinel_baseline_t;

mtfs_error_t mtfs_sentinel_baseline_init(mtfs_sentinel_baseline_t *context,
    const mtfs_sentinel_baseline_policy_t *policy);
mtfs_error_t mtfs_sentinel_baseline_start(mtfs_sentinel_baseline_t *context,
    uint32_t media_generation);
void mtfs_sentinel_baseline_reset(mtfs_sentinel_baseline_t *context);
void mtfs_sentinel_baseline_invalidate(mtfs_sentinel_baseline_t *context);
mtfs_error_t mtfs_sentinel_baseline_observe(mtfs_sentinel_baseline_t *context,
    uint32_t media_generation,
    const uint32_t raw[MTFS_SENTINEL_BASELINE_FEATURE_COUNT],
    uint8_t eligible, uint8_t injection_active);
mtfs_error_t mtfs_sentinel_baseline_transform(
    const mtfs_sentinel_baseline_t *context,
    const uint32_t raw[MTFS_SENTINEL_BASELINE_FEATURE_COUNT],
    mtfs_sentinel_baseline_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_ENABLE_STORAGE_SENTINEL */
#endif /* MTFS_SENTINEL_BASELINE_H */
