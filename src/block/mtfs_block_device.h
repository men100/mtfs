/* Platform-independent block device API for microT-FS. */
#ifndef MTFS_BLOCK_DEVICE_H
#define MTFS_BLOCK_DEVICE_H

#include <stdint.h>

#include "../mtfs_config.h"
#include "../mtfs_error.h"
#include "../mtfs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t mtfs_block_status_t;
typedef uint32_t mtfs_block_capabilities_t;

#define MTFS_BLOCK_STATUS_INITIALIZED      (UINT32_C(1) << 0)
#define MTFS_BLOCK_STATUS_MEDIA_PRESENT    (UINT32_C(1) << 1)
#define MTFS_BLOCK_STATUS_WRITE_PROTECTED  (UINT32_C(1) << 2)

#define MTFS_BLOCK_CAPABILITY_READ_ONLY    (UINT32_C(1) << 0)
#define MTFS_BLOCK_CAPABILITY_TRIM         (UINT32_C(1) << 1)
#define MTFS_BLOCK_CAPABILITY_DIAGNOSTICS  (UINT32_C(1) << 2)

typedef struct mtfs_block_geometry
{
    uint32_t sector_size;
    mtfs_lba_t sector_count;
    uint32_t erase_block_size;
} mtfs_block_geometry_t;

typedef struct mtfs_block_device_ops
{
    mtfs_error_t (*initialize)(void *context);
    mtfs_error_t (*status)(void *context, mtfs_block_status_t *status);
    mtfs_error_t (*read)(void *context, void *buffer, mtfs_lba_t lba, uint32_t count);
    mtfs_error_t (*write)(void *context, const void *buffer, mtfs_lba_t lba, uint32_t count);
    mtfs_error_t (*sync)(void *context);
    mtfs_error_t (*get_geometry)(void *context, mtfs_block_geometry_t *geometry);
    mtfs_error_t (*trim)(void *context, mtfs_lba_t lba, mtfs_lba_t count);
} mtfs_block_device_ops_t;

struct mtfs_block_device
{
    const mtfs_block_device_ops_t *ops;
    void *context;
    mtfs_block_capabilities_t capabilities;
#if MTFS_ENABLE_DIAGNOSTICS
    struct mtfs_block_diagnostics_state *diagnostics;
#endif
};

int mtfs_block_device_is_valid(const mtfs_block_device_t *device);
mtfs_error_t mtfs_block_initialize(mtfs_block_device_t *device);
mtfs_error_t mtfs_block_status(mtfs_block_device_t *device, mtfs_block_status_t *status);
mtfs_error_t mtfs_block_read(mtfs_block_device_t *device, void *buffer, mtfs_lba_t lba, uint32_t count);
mtfs_error_t mtfs_block_write(mtfs_block_device_t *device, const void *buffer, mtfs_lba_t lba, uint32_t count);
mtfs_error_t mtfs_block_sync(mtfs_block_device_t *device);
mtfs_error_t mtfs_block_get_geometry(mtfs_block_device_t *device, mtfs_block_geometry_t *geometry);
mtfs_error_t mtfs_block_trim(mtfs_block_device_t *device, mtfs_lba_t lba, mtfs_lba_t count);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_BLOCK_DEVICE_H */
