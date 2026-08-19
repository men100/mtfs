#ifndef MTFS_RA8P1_VECTOR_CACHE_H
#define MTFS_RA8P1_VECTOR_CACHE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MTFS_RA8P1_DISABLE_CACHES_FALLBACK
#define MTFS_RA8P1_DISABLE_CACHES_FALLBACK (0)
#endif

typedef struct mtfs_ra8p1_vector_cache_diagnostics
{
    uint32_t ccr_at_hal_entry;
    uint32_t vtor_at_hal_entry;
    uint32_t vtor_after_relocation;
    uint32_t vector_start;
    uint32_t vector_size;
    uint32_t dcache_line_size;
    uint32_t clean_count;
    uint32_t fallback_active;
} mtfs_ra8p1_vector_cache_diagnostics_t;

typedef struct mtfs_ra8p1_fault_snapshot
{
    uint32_t valid;
    uint32_t exception_number;
    uint32_t exc_return;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t mmfar;
    uint32_t bfar;
    uint32_t shcsr;
    uint32_t vtor;
    uint32_t msp;
    uint32_t psp;
    uint32_t stacked_lr;
    uint32_t stacked_pc;
    uint32_t stacked_xpsr;
} mtfs_ra8p1_fault_snapshot_t;

extern volatile mtfs_ra8p1_vector_cache_diagnostics_t
    g_mtfs_ra8p1_vector_cache_diagnostics;
extern volatile mtfs_ra8p1_fault_snapshot_t g_mtfs_ra8p1_fault_snapshot;

void mtfs_ra8p1_capture_cache_startup(void);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_RA8P1_VECTOR_CACHE_H */
