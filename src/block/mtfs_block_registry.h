/* Fixed-size physical-drive registry for microT-FS block devices. */
#ifndef MTFS_BLOCK_REGISTRY_H
#define MTFS_BLOCK_REGISTRY_H

#include <stdint.h>

#include "../mtfs_config.h"
#include "mtfs_block_device.h"

#ifdef __cplusplus
extern "C" {
#endif

mtfs_error_t mtfs_block_registry_register(uint32_t physical_drive, mtfs_block_device_t *device);
mtfs_error_t mtfs_block_registry_get(uint32_t physical_drive, mtfs_block_device_t **device);
mtfs_error_t mtfs_block_registry_unregister(uint32_t physical_drive);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_BLOCK_REGISTRY_H */
