#include "hal_data.h"
#include "mtfs_ra8p1_vector_cache.h"

void hal_entry(void)
{
    mtfs_ra8p1_capture_cache_startup();

#if MTFS_RA8P1_DISABLE_CACHES_FALLBACK
    /* Diagnostic fallback only; normal builds keep both caches enabled. */
#if (__DCACHE_PRESENT == 1U)
    SCB_DisableDCache();
#endif
#if (__ICACHE_PRESENT == 1U)
    SCB_DisableICache();
#endif
    __DSB();
    __ISB();
#endif

    void knl_start_mtkernel(void);
    knl_start_mtkernel();
}
