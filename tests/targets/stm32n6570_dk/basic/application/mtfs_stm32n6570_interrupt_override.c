/*
 * STM32 Armv8-M interrupt override for mtk3_bsp2 v1.00.04.
 *
 * v1.00.04 contains an sN_INTVEC typo and does not make writes to the RAM
 * vector table coherent while D/I cache is enabled.  The CubeIDE project
 * excludes only the corresponding submodule translation unit and builds this
 * target-local replacement.  The submodule itself remains untouched.
 */
#include <sys/machine.h>

#if defined(MTKBSP_STM32CUBE) && defined(MTKBSP_CPU_CORE_ARMV8M)

#include <tk/tkernel.h>
#include <kernel.h>
#include <stddef.h>
#include <stdint.h>
#include "stm32n6xx.h"
#include "sysdepend/stm32_cube/cpu/core/armv8m/sysdepend.h"
#include "sysdepend/stm32_cube/cpu/core/armv8m/cpu_status.h"

#define MTFS_STM32N6_CACHE_LINE_SIZE (32U)

LOCAL UW hllint_tbl[N_INTVEC];

static void mtfs_stm32n6_sync_vector_range(const void *address, size_t size)
{
    uintptr_t first = (uintptr_t)address &
        ~(uintptr_t)(MTFS_STM32N6_CACHE_LINE_SIZE - 1U);
    uintptr_t last = ((uintptr_t)address + size +
        MTFS_STM32N6_CACHE_LINE_SIZE - 1U) &
        ~(uintptr_t)(MTFS_STM32N6_CACHE_LINE_SIZE - 1U);

    SCB_CleanDCache_by_Addr((uint32_t *)first, (int32_t)(last - first));
    __DSB();
    SCB_InvalidateICache();
    __DSB();
    __ISB();
}

EXPORT void knl_hll_inthdr(void)
{
    FP handler;
    UW interrupt_number;

    ENTER_TASK_INDEPENDENT;
    interrupt_number = knl_get_ipsr() - 16U;
    handler = (FP)hllint_tbl[interrupt_number];
    (*(void (*)(UW))handler)(interrupt_number);
    LEAVE_TASK_INDEPENDENT;
}

EXPORT void knl_systim_inthdr(void)
{
    ENTER_TASK_INDEPENDENT;
    knl_timer_handler();
    LEAVE_TASK_INDEPENDENT;
}

EXPORT ER knl_define_inthdr(INT interrupt_number, ATR attributes, FP handler)
{
    volatile FP *vector;

    if (handler != NULL) {
        if ((attributes & TA_HLNG) != 0U) {
            hllint_tbl[interrupt_number] = (UW)handler;
            handler = knl_hll_inthdr;
        }
    } else {
        handler = (FP)knl_exctbl_o[N_SYSVEC + interrupt_number];
    }
    vector = (FP *)(knl_exctbl + N_SYSVEC);
    vector[interrupt_number] = handler;

    mtfs_stm32n6_sync_vector_range(
        (const void *)&vector[interrupt_number], sizeof(handler));
    return E_OK;
}

EXPORT void knl_return_inthdr(void)
{
}

EXPORT ER knl_init_interrupt(void)
{
    knl_exctbl[2] = (UW)knl_nmi_handler;
    knl_exctbl[3] = (UW)knl_hardfault_handler;
    knl_exctbl[4] = (UW)knl_memmanage_handler;
    knl_exctbl[5] = (UW)knl_busfault_handler;
    knl_exctbl[6] = (UW)knl_usagefault_handler;
    knl_exctbl[11] = (UW)knl_svcall_handler;
    knl_exctbl[12] = (UW)knl_debugmon_handler;
    knl_exctbl[14] = (UW)knl_dispatch_entry;
    knl_exctbl[15] = (UW)knl_systim_inthdr;

    /* Also publishes the external vectors copied before this hook runs. */
    mtfs_stm32n6_sync_vector_range(knl_exctbl,
        sizeof(UW) * (N_SYSVEC + N_INTVEC));
    return E_OK;
}

#endif /* MTKBSP_STM32CUBE && MTKBSP_CPU_CORE_ARMV8M */
