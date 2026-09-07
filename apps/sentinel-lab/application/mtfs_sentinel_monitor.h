#ifndef MTFS_SENTINEL_MONITOR_H
#define MTFS_SENTINEL_MONITOR_H

#include <stddef.h>
#include <stdint.h>

#include "mtfs_config.h"

#if MTFS_ENABLE_STORAGE_SENTINEL && MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE

#include "mtfs_sentinel_inference.h"
#include "mtfs_sentinel_lab_window.h"

typedef enum mtfs_sentinel_monitor_media_state
{
    MTFS_SENTINEL_MONITOR_MEDIA_UNKNOWN = 0,
    MTFS_SENTINEL_MONITOR_MEDIA_PRESENT = 1,
    MTFS_SENTINEL_MONITOR_MEDIA_ABSENT = 2,
    MTFS_SENTINEL_MONITOR_MEDIA_NOT_READY = 3
} mtfs_sentinel_monitor_media_state_t;

typedef enum mtfs_sentinel_monitor_source
{
    MTFS_SENTINEL_MONITOR_SOURCE_RULE = 0,
    MTFS_SENTINEL_MONITOR_SOURCE_NPU = 1,
    MTFS_SENTINEL_MONITOR_SOURCE_CPU_ARBITRATION = 2,
    MTFS_SENTINEL_MONITOR_SOURCE_CPU_FALLBACK = 3
} mtfs_sentinel_monitor_source_t;

typedef enum mtfs_sentinel_monitor_state
{
    MTFS_SENTINEL_MONITOR_STATE_NORMAL = 0,
    MTFS_SENTINEL_MONITOR_STATE_ANOMALY = 1,
    MTFS_SENTINEL_MONITOR_STATE_NO_MEDIA = 2,
    MTFS_SENTINEL_MONITOR_STATE_NOT_READY = 3,
    MTFS_SENTINEL_MONITOR_STATE_DISCONTINUITY = 4,
    MTFS_SENTINEL_MONITOR_STATE_INSUFFICIENT = 5,
    MTFS_SENTINEL_MONITOR_STATE_INVALID = 6,
    MTFS_SENTINEL_MONITOR_STATE_BLOCK_ERROR = 7,
    MTFS_SENTINEL_MONITOR_STATE_INFERENCE_ERROR = 8,
    MTFS_SENTINEL_MONITOR_STATE_WARMUP = 9,
    MTFS_SENTINEL_MONITOR_STATE_OUT_OF_DISTRIBUTION = 10
} mtfs_sentinel_monitor_state_t;

typedef struct mtfs_sentinel_monitor_preprocessing_status
{
    uint32_t saturation_mask;
    uint16_t baseline_progress;
    uint16_t baseline_required;
} mtfs_sentinel_monitor_preprocessing_status_t;

typedef struct mtfs_sentinel_monitor_provider_ops
{
    mtfs_error_t (*open)(void *context, uint64_t *threshold_q8);
    mtfs_error_t (*normalize)(void *context,
        const mtfs_sentinel_feature_v1_t *feature,
        int8_t input_q4[MTFS_SENTINEL_FEATURE_DIMENSION]);
    mtfs_error_t (*npu_infer)(void *context,
        const int8_t input_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
        int8_t output_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
        mtfs_sentinel_inference_result_t *result, uint32_t *latency_us);
    mtfs_error_t (*cpu_infer)(void *context,
        const int8_t input_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
        mtfs_sentinel_inference_result_t *result, uint32_t *latency_us);
    mtfs_error_t (*close)(void *context);
    void (*preprocessing_status)(void *context,
        mtfs_sentinel_monitor_preprocessing_status_t *status);
} mtfs_sentinel_monitor_provider_ops_t;

typedef struct mtfs_sentinel_monitor_input
{
    mtfs_sentinel_lab_window_result_t window;
    mtfs_sentinel_monitor_media_state_t media_state;
} mtfs_sentinel_monitor_input_t;

typedef struct mtfs_sentinel_monitor_result
{
    uint32_t sequence;
    uint32_t media_generation;
    uint64_t validity_mask;
    uint32_t flags;
    uint64_t score_q8;
    uint64_t score_min_q8;
    uint64_t score_max_q8;
    uint64_t threshold_q8;
    uint32_t inference_latency_us;
    mtfs_error_t inference_status;
    mtfs_sentinel_monitor_media_state_t media_state;
    mtfs_sentinel_monitor_source_t source;
    mtfs_sentinel_monitor_state_t state;
    uint8_t cpu_arbitrated;
    uint16_t baseline_progress;
    uint16_t baseline_required;
    uint32_t saturation_mask;
} mtfs_sentinel_monitor_result_t;

typedef struct mtfs_sentinel_monitor_diagnostics
{
    uint32_t open_calls;
    uint32_t close_calls;
    uint32_t windows;
    uint32_t npu_inferences;
    uint32_t cpu_arbitrations;
    uint32_t cpu_fallbacks;
    uint32_t rule_decisions;
    uint32_t warmup_windows;
    uint32_t out_of_distribution;
    uint32_t failures;
} mtfs_sentinel_monitor_diagnostics_t;

typedef struct mtfs_sentinel_monitor
{
    const mtfs_sentinel_monitor_provider_ops_t *ops;
    void *provider_context;
    uint64_t threshold_q8;
    uint32_t maximum_q4_error;
    uint32_t previous_media_generation;
    uint8_t previous_generation_valid;
    uint8_t open;
    mtfs_sentinel_monitor_diagnostics_t diagnostics;
} mtfs_sentinel_monitor_t;

typedef mtfs_error_t (*mtfs_sentinel_monitor_acquire_fn)(void *context,
    uint32_t stage, uint32_t index, mtfs_sentinel_monitor_input_t *input);
typedef mtfs_error_t (*mtfs_sentinel_monitor_stage_fn)(void *context,
    uint32_t stage);
typedef const char *(*mtfs_sentinel_monitor_stage_name_fn)(void *context,
    uint32_t stage);
typedef void (*mtfs_sentinel_monitor_write_fn)(void *context,
    const char *text);

typedef struct mtfs_sentinel_monitor_run_config
{
    void *context;
    const mtfs_sentinel_monitor_provider_ops_t *provider_ops;
    void *provider_context;
    mtfs_sentinel_monitor_acquire_fn acquire;
    mtfs_sentinel_monitor_stage_fn stage_begin;
    mtfs_sentinel_monitor_stage_fn stage_end;
    mtfs_sentinel_monitor_stage_name_fn stage_name;
    mtfs_sentinel_monitor_write_fn write;
    uint32_t stage_count;
    uint32_t samples_per_stage;
    uint32_t warmup_samples;
    uint32_t maximum_q4_error;
} mtfs_sentinel_monitor_run_config_t;

mtfs_error_t mtfs_sentinel_monitor_open(mtfs_sentinel_monitor_t *monitor,
    const mtfs_sentinel_monitor_provider_ops_t *ops, void *provider_context,
    uint32_t maximum_q4_error);
mtfs_error_t mtfs_sentinel_monitor_evaluate(mtfs_sentinel_monitor_t *monitor,
    const mtfs_sentinel_monitor_input_t *input,
    mtfs_sentinel_monitor_result_t *result);
mtfs_error_t mtfs_sentinel_monitor_close(mtfs_sentinel_monitor_t *monitor);
mtfs_error_t mtfs_sentinel_monitor_format_line(char *buffer, size_t capacity,
    const mtfs_sentinel_monitor_input_t *input,
    const mtfs_sentinel_monitor_result_t *result);
int mtfs_sentinel_monitor_run(mtfs_sentinel_monitor_t *monitor,
    const mtfs_sentinel_monitor_run_config_t *config);

#endif
#endif
