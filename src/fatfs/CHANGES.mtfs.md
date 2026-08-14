# microT-FS changes to FatFs

## Phase 3.3 RTC timestamp bridge

- Added `mtfs_fattime.c` outside upstream `ff.c`; no upstream timestamp logic was modified.
- `MTFS_FF_FS_NORTC=1` remains the zero-dependency fixed-time default.
- With `MTFS_FF_FS_NORTC=0`, `get_fattime()` returns a packed local timestamp only for a `VALID` microT-FS provider and returns zero for every untrusted state.

This file tracks intentional microT-FS differences from the upstream baseline recorded in `UPSTREAM.md`.

The current baseline is FatFs R0.16 with official patch-1 followed by official patch-2. Those patches are upstream ChaN patches and are not microT-FS-specific changes.

### MTFS-0001: Configurable FatFs write and formatting support

- Status: applied
- Upstream files affected: `ffconf.h`
- microT-FS files affected: `mtfs_config.h`
- Motivation: Allow explicit host-test formatting without enabling formatting in the default production configuration, and allow read-only production builds without editing the imported configuration directly.
- Behavioral or compatibility impact: None with the default settings; applications can opt in to the upstream `f_mkfs()` API or compile FatFs read-only.
- Configuration impact: `FF_USE_MKFS` follows `MTFS_FF_USE_MKFS` (default `0`), and `FF_FS_READONLY` follows `MTFS_FF_FS_READONLY` (default `0`). Only the host round-trip target enables formatting.
- Tests: Host file-backed FAT format, unmount/remount and file round-trip test; compile-only read-only Disk I/O bridge target.
- Related issue or commit: Initial block-device and host round-trip implementation.

### MTFS-0002: Configurable volumes, fixed timestamps, and re-entrancy

- Status: applied
- Upstream files affected: `ffconf.h`
- microT-FS files affected: `mtfs_config.h`, `src/os/host/`, `src/os/microtkernel/`
- Motivation: Select FatFs volume count, RTC policy, timeout, and re-entrancy from the application configuration while keeping OS synchronization outside the upstream FatFs algorithm.
- Behavioral or compatibility impact: The default changes to `FF_FS_NORTC=1`, so files use FatFs fixed timestamps until a later RTC phase. Re-entrancy remains disabled by default and now requires an explicit POSIX or microT-Kernel adapter when enabled.
- Configuration impact: `FF_VOLUMES`, `FF_FS_NORTC`, `FF_FS_REENTRANT`, and `FF_FS_TIMEOUT` follow the corresponding `MTFS_FF_*` macros. `FF_VOLUMES` is compile-time checked against `MTFS_BLOCK_REGISTRY_SIZE`. `MTFS_FATFS_MUTEX_ADAPTER` selects exactly one OS implementation.
- Tests: POSIX re-entrant host format/round-trip/concurrent test, read-only Disk I/O compile-only target, non-re-entrant FatFs compile-only target, and BSP2 public-header compile-only check for the microT-Kernel adapter.
- Related issue or commit: Phase 1 FatFs re-entrancy and portable common tests.

## Change record template

### MTFS-XXXX: Short title

- Status: proposed / applied / superseded
- Upstream files affected:
- microT-FS files affected:
- Motivation:
- Behavioral or compatibility impact:
- Configuration impact:
- Tests:
- Related issue or commit:
