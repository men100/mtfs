# Renesas RA FSP RTC provider

This port exposes the FSP `r_rtc` calendar as the microT-FS local-time
provider. It is built only when `MTFS_FF_FS_NORTC=0`.

The FSP instance must use the sub-clock, configure the mandatory carry IRQ,
and set **Set Source Clock in Open** to **Disabled**. The port calls
`clockSourceSet` only when the RA8P1 `VBTBPSR.VBPORF` flag reports
backup-domain power loss. Marker absence by itself never reselects the clock,
which avoids resetting a live calendar after `clear` or an ordinary software
reset.

## RA8P1 backup marker

The port reserves `VBTBKR[116..127]`, the final 12 bytes of the 128-byte
VBATT backup area:

- `116..119`: marker version
- `120..123`: inverse of the magic value
- `124..127`: magic value, written last as the commit record

Every access sets `VBTBER.VBAE`, waits at least 500 ns, accesses the marker,
and clears `VBAE` again. The complete sequence is enclosed by FSP
`BSP_REG_PROTECT_OM_LPC_BATT` protection disable/enable calls because
`VBTBER`, `VBTBKR`, and `VBTBPSR` are protected by `PRCR.PRC1`. Leaving
`VBAE` set can prevent VBTBKR retention in VBATT mode.

The EK-RA8P1 target is currently a flat build (`BSP_TZ_SECURE_BUILD=0` and
`BSP_TZ_NONSECURE_BUILD=0`). If the target is split into TrustZone images,
the VBATT security and privilege boundaries must grant the provider access
to `VBTBER` and the complete final 32-byte VBTBKR block (`VBTBKR[96..127]`),
because RA8P1 backup-register boundaries have 32-byte granularity.

The board's J36 battery connector is not fitted by default. Software resets
retain RTC state; RTC retention across complete board power removal requires
an external VBATT source connected according to the board manual.
