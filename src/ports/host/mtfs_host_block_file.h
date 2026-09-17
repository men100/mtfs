/** @file mtfs_host_block_file.h
 * @brief Host-only file-backed Block Device adapter. / host環境専用のfile-backed Block Device adapter。
 * @details The caller owns context and backing-file lifetime. This test port is not an embedded target or a durability guarantee.
 * / contextとbacking fileのlifetimeは呼び出し側が管理する。このportはtest用途であり、組み込みtarget向けの実装やデータ永続性を保証するものではない。
 * @ingroup mtfs_ports */
#ifndef MTFS_HOST_BLOCK_FILE_H
#define MTFS_HOST_BLOCK_FILE_H

/** @addtogroup mtfs_ports
 * @{ */

#include <stdint.h>

#include "../../block/mtfs_block_device.h"
#include "../../block/mtfs_block_diagnostics.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_HOST_BLOCK_FILE_DEFAULT_SECTOR_SIZE (512U)

typedef struct mtfs_host_block_file
{
    void *stream;
    mtfs_block_geometry_t geometry;
    int is_open;
    int initialized;
    int read_only;
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_block_diagnostics_state_t block_diagnostics;
#endif
} mtfs_host_block_file_t;

mtfs_error_t mtfs_host_block_file_open(
    mtfs_host_block_file_t *context,
    mtfs_block_device_t *device,
    const char *path,
    uint32_t sector_size,
    int read_only);
mtfs_error_t mtfs_host_block_file_close(mtfs_host_block_file_t *context);

#ifdef __cplusplus
}
#endif

/** @} */
#endif /* MTFS_HOST_BLOCK_FILE_H */
