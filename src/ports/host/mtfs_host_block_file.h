/* Host-only file-backed block device port. */
#ifndef MTFS_HOST_BLOCK_FILE_H
#define MTFS_HOST_BLOCK_FILE_H

#include <stdint.h>

#include "../../block/mtfs_block_device.h"

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

#endif /* MTFS_HOST_BLOCK_FILE_H */
