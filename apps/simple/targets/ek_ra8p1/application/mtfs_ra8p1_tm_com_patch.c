#include <tk/tkernel.h>

#include <mtkernel/lib/libtm/libtm.h>

/*
 * Target-side compatibility wrapper for mtk3_bsp2 v1.00.04.
 *
 * Upstream commit 6e18f885347a485b47448d8fbb46c3a0b028431d fixes the
 * RA8P1 receive loop on the develop branch.  The pinned BSP returns without
 * writing the destination when RDRF is clear, so tm_getchar() can return an
 * indeterminate byte.  Keep the submodule unchanged and redirect references
 * with the linker's --wrap=tm_rcv_dat option.
 */

#define MTFS_RA8P1_SCI8_BASE  (UINT32_C(0x40358800))
#define MTFS_RA8P1_SCI8_RDR   (MTFS_RA8P1_SCI8_BASE + UINT32_C(0x00))
#define MTFS_RA8P1_SCI8_CSR   (MTFS_RA8P1_SCI8_BASE + UINT32_C(0x48))
#define MTFS_RA8P1_SCI8_CFCLR (MTFS_RA8P1_SCI8_BASE + UINT32_C(0x68))

#define MTFS_RA8P1_CSR_RDRF (UINT32_C(1) << 31)
#define MTFS_RA8P1_CSR_ERR  \
    ((UINT32_C(1) << 28) | (UINT32_C(1) << 27) | (UINT32_C(1) << 24))

EXPORT void __wrap_tm_rcv_dat(UB *buffer, INT size);

EXPORT void __wrap_tm_rcv_dat(UB *buffer, INT size)
{
    while (size > 0) {
        UW status = in_w(MTFS_RA8P1_SCI8_CSR);

        if ((status & MTFS_RA8P1_CSR_RDRF) != 0U) {
            *buffer++ = in_b(MTFS_RA8P1_SCI8_RDR);
            --size;
        } else if ((status & MTFS_RA8P1_CSR_ERR) != 0U) {
            out_w(MTFS_RA8P1_SCI8_CFCLR,
                status & MTFS_RA8P1_CSR_ERR);
        }
    }
}
