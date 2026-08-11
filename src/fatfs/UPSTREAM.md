# FatFs upstream record

## Baseline

| Field | Value |
| --- | --- |
| Upstream project | FatFs by ChaN |
| Upstream version / release | R0.16 |
| Baseline state | R0.16 with official patch 2 |
| Source page URL | https://elm-chan.org/fsw/ff/ |
| ZIP download URL | https://elm-chan.org/fsw/ff/arc/ff16.zip |
| Official patch page | https://elm-chan.org/fsw/ff/patches.html |
| Patch-1 URL | https://elm-chan.org/fsw/ff/patch/ff16p1.diff |
| Patch-2 URL | https://elm-chan.org/fsw/ff/patch/ff16p2.diff |
| ZIP file name | `ff16.zip` |
| Retrieved on (YYYY-MM-DD) | 2026-08-11 |
| ZIP SHA-256 | `99F7DC1F7E095356E4A9E3DBE29959090D8B948AFE2BBC5441E52FDF4B85449E` |
| Patch-1 SHA-256 | `7996DDC3135F3D534A8153F7D75D587030E5C8551B003C3B7EE823FFB7FC64BF` |
| Patch-2 SHA-256 | `5CD39F1FC299F0F1DEC9FA1CE544DC0BC048B2F6EB3B8161476ED20F9CF1E290` |

## Imported files

The following upstream files are stored directly in `src/fatfs/`:

- `source/ff.c` as `ff.c`, after applying official patch-1 and patch-2
- `source/ff.h`
- `source/ffconf.h`
- `source/diskio.h`
- `source/ffsystem.c`
- `source/ffunicode.c`
- `source/00history.txt`
- `source/00readme.txt`
- `LICENSE.txt`

The official diffs are preserved for reproducibility as:

- `patches/ff16p1.diff`
- `patches/ff16p2.diff`

## Omissions

- `source/diskio.c` is an upstream sample/template and is intentionally omitted. A later change will provide the shared microT-FS implementation as `mtfs_diskio.c`.
- The upstream `documents/` tree is intentionally omitted from the vendored core.

## Import procedure and normalization

`ff16.zip` is the authoritative archive. Its `source/ff.c` was patched with a diff application tool in this exact order:

1. Apply `ff16p1.diff` to `ff.c`, producing the patch-1 state.
2. Apply `ff16p2.diff` to that patch-1 state, producing the final `ff.c`.

Both patches applied with all hunks successful and without reject files. The final source header identifies `R0.16 w/patch 2`.

All imported text files, including the preserved patch files, were normalized to LF line endings without a byte-order mark. No source formatting, configuration, or functional changes were made beyond the two official patches and line-ending normalization.

## License provenance

The upstream `LICENSE.txt` from `ff16.zip` is preserved at `src/fatfs/LICENSE.txt`. The imported FatFs files remain subject to that upstream license.
