#ifndef MTFS_SENTINEL_RECORDER_H
#define MTFS_SENTINEL_RECORDER_H

#include <stddef.h>
#include <stdint.h>

#include "mtfs_sentinel.h"

#if MTFS_ENABLE_STORAGE_SENTINEL
typedef struct mtfs_sentinel_dataset_metadata
{
    const char *label;
    uint32_t marker;
    const char *build_type;
    const char *command;
    const char *scenario_origin;
    const char *stage;
    uint32_t severity;
    const char *injection_kind;
    uint32_t injection_operation_mask;
    uint32_t injection_rate_permille;
    uint32_t requested_delay_us;
    uint32_t actual_injection_count;
    uint32_t random_seed;
    uint32_t sequence;
} mtfs_sentinel_dataset_metadata_t;

/* CSV header/frame helpers are application-only; the Sentinel library is I/O-free. */
const char *mtfs_sentinel_recorder_csv_header(void);
mtfs_error_t mtfs_sentinel_recorder_format_csv(char *buffer, size_t capacity,
    const mtfs_sentinel_feature_v1_t *feature, const char *label,
    uint32_t marker);
mtfs_error_t mtfs_sentinel_recorder_format_dataset_csv(char *buffer,
    size_t capacity, const mtfs_sentinel_feature_v1_t *feature,
    const mtfs_sentinel_dataset_metadata_t *metadata);
#endif

/* Lab workload is also available to Sentinel-disabled performance baselines. */
typedef void (*mtfs_sentinel_recorder_cleanup_fn)(void *context);
mtfs_error_t mtfs_sentinel_recorder_workload(const char *volume,
    uint32_t marker, void *buffer, uint32_t size);
mtfs_error_t mtfs_sentinel_recorder_workload_ex(const char *volume,
    uint32_t marker, void *buffer, uint32_t size,
    mtfs_sentinel_recorder_cleanup_fn before_cleanup,
    void *cleanup_context);

#endif
