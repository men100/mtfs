#include "mtfs_block_registry.h"

#include <stddef.h>

#if MTFS_BLOCK_REGISTRY_SIZE == 0
#error MTFS_BLOCK_REGISTRY_SIZE must be greater than zero
#endif

static mtfs_block_device_t *mtfs_block_devices[MTFS_BLOCK_REGISTRY_SIZE];

mtfs_error_t mtfs_block_registry_register(
    uint32_t physical_drive,
    mtfs_block_device_t *device)
{
    if (physical_drive >= MTFS_BLOCK_REGISTRY_SIZE) {
        return MTFS_ERROR_OUT_OF_RANGE;
    }
    if (!mtfs_block_device_is_valid(device)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    if (mtfs_block_devices[physical_drive] != NULL) {
        return MTFS_ERROR_ALREADY_EXISTS;
    }
    mtfs_block_devices[physical_drive] = device;
    return MTFS_OK;
}

mtfs_error_t mtfs_block_registry_get(
    uint32_t physical_drive,
    mtfs_block_device_t **device)
{
    if (device == NULL) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    *device = NULL;
    if (physical_drive >= MTFS_BLOCK_REGISTRY_SIZE) {
        return MTFS_ERROR_OUT_OF_RANGE;
    }
    if (mtfs_block_devices[physical_drive] == NULL) {
        return MTFS_ERROR_NOT_FOUND;
    }
    *device = mtfs_block_devices[physical_drive];
    return MTFS_OK;
}

mtfs_error_t mtfs_block_registry_unregister(uint32_t physical_drive)
{
    if (physical_drive >= MTFS_BLOCK_REGISTRY_SIZE) {
        return MTFS_ERROR_OUT_OF_RANGE;
    }
    if (mtfs_block_devices[physical_drive] == NULL) {
        return MTFS_ERROR_NOT_FOUND;
    }
    mtfs_block_devices[physical_drive] = NULL;
    return MTFS_OK;
}
