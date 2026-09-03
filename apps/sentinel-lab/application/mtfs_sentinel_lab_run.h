#ifndef MTFS_SENTINEL_LAB_RUN_H
#define MTFS_SENTINEL_LAB_RUN_H

#include <stdint.h>

#include "ff.h"
#include "mtfs_sentinel.h"
#include "mtfs_sentinel_lab_injector.h"

typedef enum mtfs_sentinel_lab_mode
{
    MTFS_SENTINEL_LAB_MODE_RECORD = 0,
    MTFS_SENTINEL_LAB_MODE_DELAY_RAMP = 1,
    MTFS_SENTINEL_LAB_MODE_HARD_FAULT = 2
} mtfs_sentinel_lab_mode_t;

typedef mtfs_error_t (*mtfs_sentinel_lab_prepare_fn)(void *context,
    mtfs_block_device_t **device);
typedef void (*mtfs_sentinel_lab_finish_fn)(void *context);
typedef uint64_t (*mtfs_sentinel_lab_clock_fn)(void *context);
typedef void (*mtfs_sentinel_lab_sleep_fn)(void *context, uint32_t delay_ms);
typedef void (*mtfs_sentinel_lab_write_fn)(void *context, const char *text);
typedef void (*mtfs_sentinel_lab_metadata_fn)(void *context,
    mtfs_sentinel_sample_metadata_t *metadata);

typedef struct mtfs_sentinel_lab_run_config
{
    void *platform_context;
    mtfs_sentinel_lab_prepare_fn prepare;
    mtfs_sentinel_lab_finish_fn finish;
    mtfs_sentinel_lab_clock_fn clock_us;
    mtfs_sentinel_clock_fn sentinel_clock;
    mtfs_sentinel_lab_sleep_fn sleep;
    mtfs_sentinel_lab_write_fn write;
    mtfs_sentinel_lab_metadata_fn collect_metadata;
    mtfs_sentinel_lock_fn observer_lock;
    mtfs_sentinel_unlock_fn observer_unlock;
    void *observer_lock_context;
    mtfs_sentinel_transport_sample_fn transport_sample;
    void *transport_context;
    const char *build_type;
    uint32_t target_id;
    uint32_t transport_id;
    uint32_t light_delay_us;
    uint32_t medium_delay_us;
    uint32_t strong_delay_us;
} mtfs_sentinel_lab_run_config_t;

typedef struct mtfs_sentinel_lab_runtime
{
    mtfs_sentinel_lab_injector_t injector;
    mtfs_sentinel_observer_t observer;
    mtfs_sentinel_context_t sentinel;
    mtfs_sentinel_feature_v1_t frame;
    /* Last strong-delay frame, retained after a finite evaluation run. */
    mtfs_sentinel_feature_v1_t evaluation_frame;
    uint8_t evaluation_frame_valid;
    FATFS filesystem;
    uint8_t workload_buffer[4096];
    char csv_line[6144];
} mtfs_sentinel_lab_runtime_t;

int mtfs_sentinel_lab_run(mtfs_sentinel_lab_runtime_t *runtime,
    const mtfs_sentinel_lab_run_config_t *config,
    mtfs_sentinel_lab_mode_t mode, uint32_t samples, uint32_t seed);

#endif
