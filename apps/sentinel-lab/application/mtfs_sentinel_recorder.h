#ifndef MTFS_SENTINEL_RECORDER_H
#define MTFS_SENTINEL_RECORDER_H

#include <stddef.h>
#include <stdint.h>

#include "mtfs_sentinel.h"

/* CSV header/frame helpers are application-only; the Sentinel library is I/O-free. */
const char *mtfs_sentinel_recorder_csv_header(void);
mtfs_error_t mtfs_sentinel_recorder_format_csv(char *buffer, size_t capacity,
    const mtfs_sentinel_feature_v1_t *feature, const char *label,
    uint32_t marker);

/* Optional dataset workload. Uses CREATE_NEW and always attempts cleanup. */
mtfs_error_t mtfs_sentinel_recorder_workload(const char *volume,
    uint32_t marker, void *buffer, uint32_t size);

#endif
