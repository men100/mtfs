/* Optional passive timing observer for the microT-FS Block Device API. */
#ifndef MTFS_SENTINEL_OBSERVER_H
#define MTFS_SENTINEL_OBSERVER_H

#include <stdint.h>

#include "../mtfs_config.h"
#include "../block/mtfs_block_device.h"
#include "../block/mtfs_block_diagnostics.h"

#if MTFS_ENABLE_STORAGE_SENTINEL

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_SENTINEL_HISTOGRAM_BUCKETS (22U)
#define MTFS_SENTINEL_OBSERVER_API_VERSION (UINT16_C(1))

typedef mtfs_error_t (*mtfs_sentinel_clock_fn)(void *context, uint64_t *now_us);
typedef mtfs_error_t (*mtfs_sentinel_lock_fn)(void *context);
typedef void (*mtfs_sentinel_unlock_fn)(void *context);

typedef struct mtfs_sentinel_timing
{
    uint64_t sample_count;
    uint64_t total_latency_us;
    uint64_t histogram[MTFS_SENTINEL_HISTOGRAM_BUCKETS];
    uint64_t invalid_samples;
} mtfs_sentinel_timing_t;

#define MTFS_SENTINEL_OBSERVER_FLAG_SATURATED (UINT32_C(1) << 0)

typedef struct mtfs_sentinel_observer_snapshot
{
    uint16_t api_version;
    uint16_t struct_size;
    uint32_t reset_epoch;
    uint32_t flags;
    mtfs_sentinel_timing_t read;
    mtfs_sentinel_timing_t write;
    mtfs_sentinel_timing_t sync;
} mtfs_sentinel_observer_snapshot_t;

typedef struct mtfs_sentinel_observer_config
{
    mtfs_block_device_t *downstream;
    mtfs_sentinel_clock_fn clock;
    void *clock_context;
    mtfs_sentinel_lock_fn lock;
    mtfs_sentinel_unlock_fn unlock;
    void *lock_context;
} mtfs_sentinel_observer_config_t;

typedef struct mtfs_sentinel_observer
{
    mtfs_sentinel_observer_config_t config;
    mtfs_block_device_t block_device;
    mtfs_block_diagnostics_state_t diagnostics;
    mtfs_sentinel_observer_snapshot_t snapshot;
    uint32_t active_calls;
    uint8_t initialized;
} mtfs_sentinel_observer_t;

/* Inclusive upper bounds; the final bucket contains all larger values. */
extern const uint64_t
    mtfs_sentinel_histogram_upper_us[MTFS_SENTINEL_HISTOGRAM_BUCKETS - 1U];

mtfs_error_t mtfs_sentinel_observer_init(
    mtfs_sentinel_observer_t *observer,
    const mtfs_sentinel_observer_config_t *config);
mtfs_error_t mtfs_sentinel_observer_deinit(mtfs_sentinel_observer_t *observer);
mtfs_block_device_t *mtfs_sentinel_observer_block_device(
    mtfs_sentinel_observer_t *observer);
mtfs_error_t mtfs_sentinel_observer_get(
    mtfs_sentinel_observer_t *observer,
    mtfs_sentinel_observer_snapshot_t *snapshot);
mtfs_error_t mtfs_sentinel_observer_reset(mtfs_sentinel_observer_t *observer);
uint32_t mtfs_sentinel_histogram_bucket(uint64_t elapsed_us);

#ifdef __cplusplus
}
#endif
#endif /* MTFS_ENABLE_STORAGE_SENTINEL */
#endif /* MTFS_SENTINEL_OBSERVER_H */
