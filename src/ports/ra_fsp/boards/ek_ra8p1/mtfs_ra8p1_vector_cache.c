#include "mtfs_ra8p1_vector_cache.h"

#include <stddef.h>
#include <stdint.h>

#include <sys/machine.h>
#include <tk/tkernel.h>
#include <kernel.h>

#include "hal_data.h"
#include <sysdepend/ra_fsp/sysdepend.h>

/*
 * Target-side compatibility layer for mtk3_bsp2 v1.00.04.  The linker wraps
 * the three BSP2 entry points that write the cacheable RAM vector table.  The
 * submodule remains untouched and this file can be deleted with the linker
 * --wrap options when BSP2 provides equivalent maintenance itself.
 */

#define MTFS_VECTOR_ENTRY_COUNT ((uint32_t)(N_SYSVEC + N_INTVEC))
#define MTFS_VECTOR_USED_SIZE   (MTFS_VECTOR_ENTRY_COUNT * sizeof(UW))

IMPORT UW knl_exctbl[];
IMPORT UW *knl_exctbl_o;
IMPORT void *knl_lowmem_top;
IMPORT void *knl_lowmem_limit;
IMPORT const void *__mtk3_SYSMEM_START;

#if USE_STATIC_SYS_MEM
IMPORT UW knl_system_mem[];
#endif

volatile mtfs_ra8p1_vector_cache_diagnostics_t
    g_mtfs_ra8p1_vector_cache_diagnostics;
volatile mtfs_ra8p1_fault_snapshot_t g_mtfs_ra8p1_fault_snapshot;

uint32_t mtfs_ra8p1_cache_state_from_ccr(uint32_t ccr)
{
    uint32_t state = 0U;

    if ((ccr & SCB_CCR_IC_Msk) != 0U) {
        state |= MTFS_RA8P1_CACHE_STATE_ICACHE_ENABLED;
    }
    if ((ccr & SCB_CCR_DC_Msk) != 0U) {
        state |= MTFS_RA8P1_CACHE_STATE_DCACHE_ENABLED;
    }
    return state;
}

uint32_t mtfs_ra8p1_cache_state_current(void)
{
    return mtfs_ra8p1_cache_state_from_ccr(SCB->CCR);
}

static uint32_t mtfs_dcache_line_size(void)
{
#if (__DCACHE_PRESENT == 1U)
    uint32_t log2_words =
        (SCB->CTR & SCB_CTR_DMINLINE_Msk) >> SCB_CTR_DMINLINE_Pos;
    return UINT32_C(4) << log2_words;
#else
    return 0U;
#endif
}

static void mtfs_clean_vector_range(void *address, uint32_t size)
{
#if (__DCACHE_PRESENT == 1U)
    uint32_t line_size = mtfs_dcache_line_size();
    uintptr_t start = (uintptr_t)address;
    uintptr_t aligned_start;
    uintptr_t aligned_end;

    g_mtfs_ra8p1_vector_cache_diagnostics.dcache_line_size = line_size;
    if ((size == 0U) || ((SCB->CCR & SCB_CCR_DC_Msk) == 0U)) {
        __DSB();
        return;
    }

    aligned_start = start & ~((uintptr_t)line_size - 1U);
    aligned_end = (start + size + line_size - 1U) &
        ~((uintptr_t)line_size - 1U);
    SCB_CleanDCache_by_Addr((void *)aligned_start,
        (int32_t)(aligned_end - aligned_start));
    ++g_mtfs_ra8p1_vector_cache_diagnostics.clean_count;
#else
    (void)address;
    (void)size;
#endif
    __DSB();
}

void mtfs_ra8p1_capture_cache_startup(void)
{
    g_mtfs_ra8p1_vector_cache_diagnostics.ccr_at_hal_entry = SCB->CCR;
    g_mtfs_ra8p1_vector_cache_diagnostics.vtor_at_hal_entry = SCB->VTOR;
    g_mtfs_ra8p1_vector_cache_diagnostics.vector_start =
        (uint32_t)(uintptr_t)knl_exctbl;
    g_mtfs_ra8p1_vector_cache_diagnostics.vector_size =
        (uint32_t)MTFS_VECTOR_USED_SIZE;
    g_mtfs_ra8p1_vector_cache_diagnostics.dcache_line_size =
        mtfs_dcache_line_size();
    g_mtfs_ra8p1_vector_cache_diagnostics.fallback_active =
        MTFS_RA8P1_DISABLE_CACHES_FALLBACK ? 1U : 0U;
}

static void mtfs_ra8p1_fault_capture(uint32_t *stack_pointer,
    uint32_t exc_return) __attribute__((noreturn, noinline, used));

static void mtfs_ra8p1_fault_capture(uint32_t *stack_pointer,
    uint32_t exc_return)
{
    uint32_t *basic_frame = stack_pointer;

    /* EXC_RETURN bit 4 is clear when an extended FP frame precedes this. */
    if ((exc_return & UINT32_C(0x10)) == 0U) {
        basic_frame += 18;
    }

    g_mtfs_ra8p1_fault_snapshot.valid = 0U;
    g_mtfs_ra8p1_fault_snapshot.exception_number = __get_IPSR();
    g_mtfs_ra8p1_fault_snapshot.exc_return = exc_return;
    g_mtfs_ra8p1_fault_snapshot.cfsr = SCB->CFSR;
    g_mtfs_ra8p1_fault_snapshot.hfsr = SCB->HFSR;
    g_mtfs_ra8p1_fault_snapshot.mmfar = SCB->MMFAR;
    g_mtfs_ra8p1_fault_snapshot.bfar = SCB->BFAR;
    g_mtfs_ra8p1_fault_snapshot.shcsr = SCB->SHCSR;
    g_mtfs_ra8p1_fault_snapshot.vtor = SCB->VTOR;
    g_mtfs_ra8p1_fault_snapshot.msp = __get_MSP();
    g_mtfs_ra8p1_fault_snapshot.psp = __get_PSP();
    g_mtfs_ra8p1_fault_snapshot.stacked_lr = basic_frame[5];
    g_mtfs_ra8p1_fault_snapshot.stacked_pc = basic_frame[6];
    g_mtfs_ra8p1_fault_snapshot.stacked_xpsr = basic_frame[7];
    __DSB();
    g_mtfs_ra8p1_fault_snapshot.valid = UINT32_C(0x4D544653); /* MTFS */
    mtfs_clean_vector_range((void *)&g_mtfs_ra8p1_fault_snapshot,
        sizeof(g_mtfs_ra8p1_fault_snapshot));

    for (;;) {
        __BKPT(0);
    }
}

void mtfs_ra8p1_fault_entry(void) __attribute__((naked, used));

void mtfs_ra8p1_fault_entry(void)
{
    __asm volatile (
        "mov r1, lr\n"
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b mtfs_ra8p1_fault_capture\n");
}

ER __real_knl_init_interrupt(void);
ER __real_knl_define_inthdr(INT intno, ATR intatr, FP inthdr);
ER __wrap_knl_init_interrupt(void);
ER __wrap_knl_define_inthdr(INT intno, ATR intatr, FP inthdr);
void __wrap_knl_start_mtkernel(void);

ER __wrap_knl_init_interrupt(void)
{
    ER error = __real_knl_init_interrupt();

    if (error == E_OK) {
        /* Install a debugger-oriented snapshot handler for configurable faults. */
        knl_exctbl[3] = (UW)mtfs_ra8p1_fault_entry;
        knl_exctbl[4] = (UW)mtfs_ra8p1_fault_entry;
        knl_exctbl[5] = (UW)mtfs_ra8p1_fault_entry;
        knl_exctbl[6] = (UW)mtfs_ra8p1_fault_entry;
        mtfs_clean_vector_range(knl_exctbl, MTFS_VECTOR_USED_SIZE);
        __ISB();
    }
    return error;
}

ER __wrap_knl_define_inthdr(INT intno, ATR intatr, FP inthdr)
{
    ER error = __real_knl_define_inthdr(intno, intatr, inthdr);

    if ((error == E_OK) && (intno >= 0) && ((uint32_t)intno < N_INTVEC)) {
        mtfs_clean_vector_range(&knl_exctbl[N_SYSVEC + (uint32_t)intno],
            sizeof(UW));
        __ISB();
    }
    return error;
}

void __wrap_knl_start_mtkernel(void)
{
    UW *source;
    UW *destination;
    UW register_value;
    INT i;

    disint();
    knl_startup_hw();

    source = knl_exctbl_o = (UW *)(uintptr_t)in_w(SCB_VTOR);
    destination = knl_exctbl;
    for (i = 0; i < (N_SYSVEC + N_INTVEC); ++i) {
        *destination++ = *source++;
    }

    /* The clean must precede VTOR. Vector fetches do not require I-cache work. */
    mtfs_clean_vector_range(knl_exctbl, MTFS_VECTOR_USED_SIZE);
    __DSB();
    out_w(SCB_VTOR, (UW)(uintptr_t)knl_exctbl);
    __DSB();
    __ISB();
    g_mtfs_ra8p1_vector_cache_diagnostics.vtor_after_relocation = SCB->VTOR;

    register_value = *(_UW *)SCB_AIRCR;
    register_value = (register_value & ~((UW)AIRCR_PRIGROUP3)) |
        AIRCR_PRIGROUP0;
    *(_UW *)SCB_AIRCR =
        (register_value & UINT32_C(0x0000FFFF)) | AIRCR_VECTKEY;

    out_w(SCB_SHCSR,
        SHCSR_USGFAULTENA | SHCSR_BUSFAULTENA | SHCSR_MEMFAULTENA);
    out_w(SCB_SHPR2, SCB_SHPR2_VAL);
    out_w(SCB_SHPR3, SCB_SHPR3_VAL);

#if USE_IMALLOC
#if USE_STATIC_SYS_MEM
    knl_lowmem_top = knl_system_mem;
    knl_lowmem_limit = &knl_system_mem[SYSTEM_MEM_SIZE / sizeof(UW)];
#else
    if (INTERNAL_RAM_START > SYSTEMAREA_TOP) {
        knl_lowmem_top = (UW *)INTERNAL_RAM_START;
    } else {
        knl_lowmem_top = (UW *)SYSTEMAREA_TOP;
    }
    if ((UW)(uintptr_t)knl_lowmem_top <
        (UW)(uintptr_t)&__mtk3_SYSMEM_START) {
        knl_lowmem_top = (UW *)(uintptr_t)&__mtk3_SYSMEM_START;
    }

    if ((SYSTEMAREA_END != 0) &&
        (INTERNAL_RAM_END > CNF_SYSTEMAREA_END)) {
        knl_lowmem_limit = (UW *)(SYSTEMAREA_END - EXC_STACK_SIZE);
    } else {
        knl_lowmem_limit = (UW *)(INTERNAL_RAM_END - EXC_STACK_SIZE);
    }
#endif

#if USE_DEBUG_SYSMEMINFO
    knl_sysmem_top = knl_lowmem_top;
    knl_sysmem_end = knl_lowmem_limit;
#endif
#endif

    Asm ("msr msplim, %0" : : "r" ((uint32_t)INTERNAL_RAM_START));
    knl_main();
    for (;;) {
        /* knl_main() does not return. */
    }
}
