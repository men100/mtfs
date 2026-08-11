# microT-FS changes to FatFs

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
