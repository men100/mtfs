#ifndef MTFS_RA8P1_SENTINEL_RUNTIME_H
#define MTFS_RA8P1_SENTINEL_RUNTIME_H

#include <stdint.h>

#include "mtfs_error.h"

#ifdef __cplusplus
extern "C" {
#endif

int mtfs_ra8p1_sentinel_runtime_init(void);
void mtfs_ra8p1_sentinel_runtime_shutdown(void);
int mtfs_ra8p1_sentinel_rsip_lock(void *context);
void mtfs_ra8p1_sentinel_rsip_unlock(void *context);
void mtfs_ra8p1_sentinel_rsip_lock_diagnostics(int32_t *last_lock_status,
    int32_t *last_unlock_status, uint32_t *lock_attempts,
    uint32_t *unlock_attempts);
void mtfs_ra8p1_sentinel_rsip_memory_diagnostics(uint32_t *capacity,
    int *verified);
mtfs_error_t mtfs_ra8p1_sentinel_npu_lock(void *context,
    uint32_t timeout_ms);
void mtfs_ra8p1_sentinel_npu_unlock(void *context);

#ifdef __cplusplus
}
#endif

#endif
