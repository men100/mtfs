/* Storage Sentinel v1 integer feature schema and caller-driven sampling. */
/* Storage Sentinel v1整数feature schemaとcaller主導sampling。 */
#ifndef MTFS_SENTINEL_H
#define MTFS_SENTINEL_H

#include <stdint.h>

#include "../mtfs_config.h"
#include "mtfs_sentinel_observer.h"

#if MTFS_ENABLE_STORAGE_SENTINEL

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_SENTINEL_SCHEMA_VERSION (UINT16_C(1))
#define MTFS_SENTINEL_OPERATION_COUNT (3U)
#define MTFS_SENTINEL_OPERATION_READ  (0U)
#define MTFS_SENTINEL_OPERATION_WRITE (1U)
#define MTFS_SENTINEL_OPERATION_SYNC  (2U)

#define MTFS_SENTINEL_VALID_INTERVAL       (UINT64_C(1) << 0)
#define MTFS_SENTINEL_VALID_IDENTITY       (UINT64_C(1) << 1)
#define MTFS_SENTINEL_VALID_MEDIA          (UINT64_C(1) << 2)
#define MTFS_SENTINEL_VALID_IO_COUNTERS    (UINT64_C(1) << 3)
#define MTFS_SENTINEL_VALID_ERRORS         (UINT64_C(1) << 4)
#define MTFS_SENTINEL_VALID_TIMING_READ    (UINT64_C(1) << 5)
#define MTFS_SENTINEL_VALID_TIMING_WRITE   (UINT64_C(1) << 6)
#define MTFS_SENTINEL_VALID_TIMING_SYNC    (UINT64_C(1) << 7)
#define MTFS_SENTINEL_VALID_TRANSPORT      (UINT64_C(1) << 8)

/* Common inference inputs; transport fields remain explicitly optional. */
#define MTFS_SENTINEL_VALID_REQUIRED \
    (MTFS_SENTINEL_VALID_INTERVAL | MTFS_SENTINEL_VALID_IDENTITY | \
     MTFS_SENTINEL_VALID_MEDIA | MTFS_SENTINEL_VALID_IO_COUNTERS | \
     MTFS_SENTINEL_VALID_ERRORS | MTFS_SENTINEL_VALID_TIMING_READ | \
     MTFS_SENTINEL_VALID_TIMING_WRITE | MTFS_SENTINEL_VALID_TIMING_SYNC)

#define MTFS_SENTINEL_FLAG_NO_ACTIVITY        (UINT32_C(1) << 0)
#define MTFS_SENTINEL_FLAG_INSUFFICIENT_DATA  (UINT32_C(1) << 1)
#define MTFS_SENTINEL_FLAG_DISCONTINUITY      (UINT32_C(1) << 2)
#define MTFS_SENTINEL_FLAG_COUNTER_SATURATED  (UINT32_C(1) << 3)
#define MTFS_SENTINEL_FLAG_TIMING_UNAVAILABLE (UINT32_C(1) << 4)

typedef struct mtfs_sentinel_operation_feature
{
    uint64_t calls;
    uint64_t successes;
    uint64_t failures;
    uint64_t sectors_requested;
    uint64_t sectors_completed;
    uint32_t average_sectors_per_request_q16;
    uint32_t sector_completion_permille;
    uint64_t timing_samples;
    uint64_t timing_invalid;
    uint64_t total_latency_us;
    uint64_t average_latency_us;
    uint64_t latency_histogram[MTFS_SENTINEL_HISTOGRAM_BUCKETS];
} mtfs_sentinel_operation_feature_t;

#define MTFS_SENTINEL_TRANSPORT_VALID_TRANSPORT_ERRORS \
    (UINT32_C(1) << 0)
#define MTFS_SENTINEL_TRANSPORT_VALID_TRANSFER_TIMEOUTS \
    (UINT32_C(1) << 1)
#define MTFS_SENTINEL_TRANSPORT_VALID_READY_TIMEOUTS \
    (UINT32_C(1) << 2)
#define MTFS_SENTINEL_TRANSPORT_VALID_ABORTS \
    (UINT32_C(1) << 3)
#define MTFS_SENTINEL_TRANSPORT_VALID_CLOCK_ERRORS \
    (UINT32_C(1) << 4)
#define MTFS_SENTINEL_TRANSPORT_VALID_ALL \
    (MTFS_SENTINEL_TRANSPORT_VALID_TRANSPORT_ERRORS | \
     MTFS_SENTINEL_TRANSPORT_VALID_TRANSFER_TIMEOUTS | \
     MTFS_SENTINEL_TRANSPORT_VALID_READY_TIMEOUTS | \
     MTFS_SENTINEL_TRANSPORT_VALID_ABORTS | \
     MTFS_SENTINEL_TRANSPORT_VALID_CLOCK_ERRORS)

#define MTFS_SENTINEL_TRANSPORT_FLAG_COUNTERS_SATURATE \
    (UINT32_C(1) << 0)
#define MTFS_SENTINEL_TRANSPORT_FLAG_COUNTER_SATURATED \
    (UINT32_C(1) << 1)

typedef struct mtfs_sentinel_transport_snapshot
{
    uint32_t reset_epoch;
    uint32_t validity_mask;
    uint32_t flags;
    uint64_t transport_errors;
    uint64_t transfer_timeouts;
    uint64_t ready_timeouts;
    uint64_t aborts;
    uint64_t clock_errors;
} mtfs_sentinel_transport_snapshot_t;

typedef struct mtfs_sentinel_transport_feature
{
    uint32_t reset_epoch;
    uint32_t validity_mask;
    uint32_t flags;
    uint64_t transport_errors;
    uint64_t transfer_timeouts;
    uint64_t ready_timeouts;
    uint64_t aborts;
    uint64_t clock_errors;
} mtfs_sentinel_transport_feature_t;

/* Adapter-owned cumulative snapshot; the Sentinel core emits checked deltas. */
/* adapter所有の累積snapshotからSentinel coreが検査済みdeltaを生成する。 */
typedef mtfs_error_t (*mtfs_sentinel_transport_sample_fn)(
    void *context, mtfs_sentinel_transport_snapshot_t *snapshot);

typedef struct mtfs_sentinel_feature_v1
{
    uint16_t version;
    uint16_t struct_size;
    uint64_t validity_mask;
    uint32_t flags;
    uint32_t sample_count;
    uint64_t timestamp_us;
    uint64_t observation_interval_us;
    uint32_t target_id;
    uint32_t transport_id;
    uint32_t media_generation;
    uint32_t diagnostics_reset_epoch;
    uint32_t observer_reset_epoch;
    uint32_t inserted_events;
    uint32_t removed_events;
    uint32_t media_error_events;
    mtfs_sentinel_operation_feature_t operation[MTFS_SENTINEL_OPERATION_COUNT];
    uint64_t io_errors;
    uint64_t not_ready_errors;
    uint64_t no_media_errors;
    uint64_t write_protected_errors;
    uint64_t out_of_range_errors;
    uint64_t timeout_errors;
    uint64_t other_errors;
    mtfs_sentinel_transport_feature_t transport;
} mtfs_sentinel_feature_v1_t;

/* Cached media lifecycle values supplied by the application; no I/O is done. */
/* application提供のcache済みmedia lifecycle値であり、I/Oは行わない。 */
typedef struct mtfs_sentinel_sample_metadata
{
    uint32_t media_generation;
    uint32_t media_reset_epoch;
    uint32_t inserted_events;
    uint32_t removed_events;
    uint32_t error_events;
} mtfs_sentinel_sample_metadata_t;

typedef struct mtfs_sentinel_config
{
    mtfs_sentinel_observer_t *observer;
    mtfs_sentinel_clock_fn clock;
    void *clock_context;
    uint32_t target_id;
    uint32_t transport_id;
    mtfs_sentinel_transport_sample_fn transport_sample;
    void *transport_context;
} mtfs_sentinel_config_t;

typedef struct mtfs_sentinel_context
{
    mtfs_sentinel_config_t config;
    mtfs_block_diagnostics_t previous_diagnostics;
    mtfs_sentinel_observer_snapshot_t previous_observer;
    mtfs_sentinel_transport_snapshot_t previous_transport;
    mtfs_sentinel_sample_metadata_t previous_media;
    uint64_t previous_timestamp_us;
    uint32_t sample_sequence;
    uint32_t previous_target_id;
    uint32_t previous_transport_id;
    uint8_t baseline_valid;
    uint8_t previous_transport_valid;
} mtfs_sentinel_context_t;

typedef struct mtfs_sentinel_window
{
    mtfs_sentinel_feature_v1_t *storage;
    uint32_t capacity;
    uint32_t count;
    uint32_t next;
} mtfs_sentinel_window_t;

mtfs_error_t mtfs_sentinel_init(
    mtfs_sentinel_context_t *context, const mtfs_sentinel_config_t *config);
mtfs_error_t mtfs_sentinel_sample(mtfs_sentinel_context_t *context,
    const mtfs_sentinel_sample_metadata_t *metadata,
    mtfs_sentinel_feature_v1_t *feature);
/* Validate the internal count/total/average/histogram timing invariants. */
int mtfs_sentinel_operation_timing_is_consistent(
    const mtfs_sentinel_operation_feature_t *operation);
mtfs_error_t mtfs_sentinel_reset(mtfs_sentinel_context_t *context);
mtfs_error_t mtfs_sentinel_window_init(mtfs_sentinel_window_t *window,
    mtfs_sentinel_feature_v1_t *storage, uint32_t capacity);
mtfs_error_t mtfs_sentinel_window_push(mtfs_sentinel_window_t *window,
    const mtfs_sentinel_feature_v1_t *feature);
mtfs_error_t mtfs_sentinel_window_get(const mtfs_sentinel_window_t *window,
    mtfs_sentinel_feature_v1_t *feature);
void mtfs_sentinel_window_reset(mtfs_sentinel_window_t *window);

#ifdef __cplusplus
}
#endif
#endif /* MTFS_ENABLE_STORAGE_SENTINEL */
#endif /* MTFS_SENTINEL_H */
