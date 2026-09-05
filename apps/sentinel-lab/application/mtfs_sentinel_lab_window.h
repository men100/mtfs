#ifndef MTFS_SENTINEL_LAB_WINDOW_H
#define MTFS_SENTINEL_LAB_WINDOW_H

#include <stdint.h>

#include "mtfs_sentinel.h"
#include "mtfs_sentinel_recorder.h"

typedef uint64_t (*mtfs_sentinel_lab_window_clock_fn)(void *context);
typedef void (*mtfs_sentinel_lab_window_sleep_fn)(void *context,
    uint32_t delay_ms);
typedef void (*mtfs_sentinel_lab_window_metadata_fn)(void *context,
    mtfs_sentinel_sample_metadata_t *metadata);
typedef mtfs_error_t (*mtfs_sentinel_lab_window_media_ready_fn)(void *context);

typedef enum mtfs_sentinel_lab_cadence
{
    MTFS_SENTINEL_LAB_CADENCE_RELATIVE = 0,
    MTFS_SENTINEL_LAB_CADENCE_ABSOLUTE = 1
} mtfs_sentinel_lab_cadence_t;

typedef struct mtfs_sentinel_lab_window_config
{
    void *context;
    mtfs_sentinel_lab_window_clock_fn clock_us;
    mtfs_sentinel_lab_window_sleep_fn sleep;
    mtfs_sentinel_lab_window_metadata_fn collect_metadata;
    mtfs_sentinel_lab_window_media_ready_fn media_ready;
    mtfs_sentinel_context_t *sentinel;
    const char *volume;
    void *workload_buffer;
    uint32_t workload_size;
    uint32_t interval_ms;
    mtfs_sentinel_lab_cadence_t cadence;
} mtfs_sentinel_lab_window_config_t;

typedef struct mtfs_sentinel_lab_window_runtime
{
    uint64_t previous_sample_us;
    uint64_t next_deadline_us;
    uint8_t deadline_valid;
} mtfs_sentinel_lab_window_runtime_t;

typedef struct mtfs_sentinel_lab_window_result
{
    mtfs_sentinel_feature_v1_t feature;
    mtfs_error_t workload_status;
    mtfs_error_t sample_status;
    uint32_t sequence;
    uint32_t workload_elapsed_us;
    uint32_t configured_interval_us;
    uint32_t actual_interval_us;
    uint32_t overrun_us;
    uint32_t skipped_deadlines;
} mtfs_sentinel_lab_window_result_t;

void mtfs_sentinel_lab_window_runtime_init(
    mtfs_sentinel_lab_window_runtime_t *runtime);
mtfs_error_t mtfs_sentinel_lab_window_step(
    mtfs_sentinel_lab_window_runtime_t *runtime,
    const mtfs_sentinel_lab_window_config_t *config, uint32_t sequence,
    mtfs_sentinel_recorder_cleanup_fn before_cleanup, void *cleanup_context,
    mtfs_sentinel_lab_window_result_t *result);

#endif
