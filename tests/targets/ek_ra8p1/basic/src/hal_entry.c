#include "hal_data.h"

void hal_entry(void)
{
    /*
     * mtk3_bsp2 v1.00.04 relocates the exception vector table to RAM but
     * does not maintain cache coherency after updating it.  On Cortex-M85,
     * stale vector entries can therefore cause an exception lockup at
     * 0xEFFFFFFE before the initial task starts.  Keep caches disabled for
     * this target until a BSP2 revision containing the vector-table cache
     * maintenance is adopted.
     */
#if (__DCACHE_PRESENT == 1U)
    SCB_DisableDCache();
#endif
#if (__ICACHE_PRESENT == 1U)
    SCB_DisableICache();
#endif
    __DSB();
    __ISB();

    void knl_start_mtkernel(void);
    knl_start_mtkernel();
}
