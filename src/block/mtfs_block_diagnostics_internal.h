#ifndef MTFS_BLOCK_DIAGNOSTICS_INTERNAL_H
#define MTFS_BLOCK_DIAGNOSTICS_INTERNAL_H

#include "../mtfs_config.h"
#include "mtfs_block_diagnostics.h"
#include "mtfs_block_device.h"

#if MTFS_ENABLE_DIAGNOSTICS
void mtfs_block_diagnostics_record_begin(
    mtfs_block_device_t *device, mtfs_block_operation_t operation,
    uint32_t sectors);
void mtfs_block_diagnostics_record_end(
    mtfs_block_device_t *device, mtfs_block_operation_t operation,
    uint32_t sectors, mtfs_error_t result, const mtfs_block_status_t *status,
    const mtfs_block_geometry_t *geometry);
#endif

#endif /* MTFS_BLOCK_DIAGNOSTICS_INTERNAL_H */
