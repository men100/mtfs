#include "mtfs_block_device.h"
#include "mtfs_block_diagnostics_internal.h"

#include <stddef.h>

static mtfs_error_t mtfs_block_check_range(
    mtfs_block_device_t *device,
    mtfs_lba_t lba,
    mtfs_lba_t count)
{
    mtfs_block_geometry_t geometry;
    mtfs_error_t result;

    if (count == 0U) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }

    result = device->ops->get_geometry(device->context, &geometry);
    if (result != MTFS_OK) {
        return result;
    }
    if ((geometry.sector_size == 0U) || (geometry.sector_count == 0U) ||
        (geometry.erase_block_size == 0U)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    if ((lba >= geometry.sector_count) || (count > (geometry.sector_count - lba))) {
        return MTFS_ERROR_OUT_OF_RANGE;
    }

    return MTFS_OK;
}

int mtfs_block_device_is_valid(const mtfs_block_device_t *device)
{
    const mtfs_block_device_ops_t *ops;

    if ((device == NULL) || (device->ops == NULL)) {
        return 0;
    }
    if ((device->capabilities &
        ~(MTFS_BLOCK_CAPABILITY_READ_ONLY | MTFS_BLOCK_CAPABILITY_TRIM |
          MTFS_BLOCK_CAPABILITY_DIAGNOSTICS)) != 0U) {
        return 0;
    }
    ops = device->ops;
    if ((ops->initialize == NULL) || (ops->status == NULL) ||
        (ops->read == NULL) || (ops->sync == NULL) ||
        (ops->get_geometry == NULL)) {
        return 0;
    }
    if (((device->capabilities & MTFS_BLOCK_CAPABILITY_READ_ONLY) == 0U) &&
        (ops->write == NULL)) {
        return 0;
    }
    if (((device->capabilities & MTFS_BLOCK_CAPABILITY_TRIM) != 0U) &&
        (ops->trim == NULL)) {
        return 0;
    }
    return 1;
}

mtfs_error_t mtfs_block_initialize(mtfs_block_device_t *device)
{
    mtfs_error_t result;
    if (!mtfs_block_device_is_valid(device)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_block_diagnostics_record_begin(
        device, MTFS_BLOCK_OPERATION_INITIALIZE, 0U);
#endif
    result = device->ops->initialize(device->context);
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_block_diagnostics_record_end(device,
        MTFS_BLOCK_OPERATION_INITIALIZE, 0U, result, NULL, NULL);
#endif
    return result;
}

mtfs_error_t mtfs_block_status(mtfs_block_device_t *device, mtfs_block_status_t *status)
{
    mtfs_error_t result;

    if (!mtfs_block_device_is_valid(device) || (status == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_block_diagnostics_record_begin(device, MTFS_BLOCK_OPERATION_STATUS, 0U);
#endif
    result = device->ops->status(device->context, status);
    if ((result == MTFS_OK) &&
        ((device->capabilities & MTFS_BLOCK_CAPABILITY_READ_ONLY) != 0U)) {
        *status |= MTFS_BLOCK_STATUS_WRITE_PROTECTED;
    }
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_block_diagnostics_record_end(device, MTFS_BLOCK_OPERATION_STATUS,
        0U, result, status, NULL);
#endif
    return result;
}

mtfs_error_t mtfs_block_get_geometry(
    mtfs_block_device_t *device,
    mtfs_block_geometry_t *geometry)
{
    mtfs_error_t result;

    if (!mtfs_block_device_is_valid(device) || (geometry == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_block_diagnostics_record_begin(
        device, MTFS_BLOCK_OPERATION_GET_GEOMETRY, 0U);
#endif
    result = device->ops->get_geometry(device->context, geometry);
    if (result != MTFS_OK) {
        goto done;
    }
    if ((geometry->sector_size == 0U) || (geometry->sector_count == 0U) ||
        (geometry->erase_block_size == 0U)) {
        result = MTFS_ERROR_INVALID_ARGUMENT;
    }
done:
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_block_diagnostics_record_end(device,
        MTFS_BLOCK_OPERATION_GET_GEOMETRY, 0U, result, NULL, geometry);
#endif
    return result;
}

mtfs_error_t mtfs_block_read(
    mtfs_block_device_t *device,
    void *buffer,
    mtfs_lba_t lba,
    uint32_t count)
{
    mtfs_error_t result;

    if (!mtfs_block_device_is_valid(device) || (buffer == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_block_diagnostics_record_begin(
        device, MTFS_BLOCK_OPERATION_READ, count);
#endif
    result = mtfs_block_check_range(device, lba, (mtfs_lba_t)count);
    if (result == MTFS_OK) {
        result = device->ops->read(device->context, buffer, lba, count);
    }
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_block_diagnostics_record_end(device, MTFS_BLOCK_OPERATION_READ,
        count, result, NULL, NULL);
#endif
    return result;
}

mtfs_error_t mtfs_block_write(
    mtfs_block_device_t *device,
    const void *buffer,
    mtfs_lba_t lba,
    uint32_t count)
{
    mtfs_error_t result;

    if (!mtfs_block_device_is_valid(device) || (buffer == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_block_diagnostics_record_begin(
        device, MTFS_BLOCK_OPERATION_WRITE, count);
#endif
    if ((device->capabilities & MTFS_BLOCK_CAPABILITY_READ_ONLY) != 0U) {
        result = MTFS_ERROR_WRITE_PROTECTED;
        goto done;
    }
    result = mtfs_block_check_range(device, lba, (mtfs_lba_t)count);
    if (result == MTFS_OK) {
        result = device->ops->write(device->context, buffer, lba, count);
    }
done:
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_block_diagnostics_record_end(device, MTFS_BLOCK_OPERATION_WRITE,
        count, result, NULL, NULL);
#endif
    return result;
}

mtfs_error_t mtfs_block_sync(mtfs_block_device_t *device)
{
    mtfs_error_t result;
    if (!mtfs_block_device_is_valid(device)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_block_diagnostics_record_begin(device, MTFS_BLOCK_OPERATION_SYNC, 0U);
#endif
    result = device->ops->sync(device->context);
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_block_diagnostics_record_end(device, MTFS_BLOCK_OPERATION_SYNC,
        0U, result, NULL, NULL);
#endif
    return result;
}

mtfs_error_t mtfs_block_trim(
    mtfs_block_device_t *device,
    mtfs_lba_t lba,
    mtfs_lba_t count)
{
    mtfs_error_t result;

    if (!mtfs_block_device_is_valid(device)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_block_diagnostics_record_begin(device, MTFS_BLOCK_OPERATION_TRIM,
        (count > UINT32_MAX) ? UINT32_MAX : (uint32_t)count);
#endif
    if ((device->capabilities & MTFS_BLOCK_CAPABILITY_READ_ONLY) != 0U) {
        result = MTFS_ERROR_WRITE_PROTECTED;
        goto done;
    }
    if (((device->capabilities & MTFS_BLOCK_CAPABILITY_TRIM) == 0U) ||
        (device->ops->trim == NULL)) {
        result = MTFS_ERROR_NOT_SUPPORTED;
        goto done;
    }
    result = mtfs_block_check_range(device, lba, count);
    if (result == MTFS_OK) {
        result = device->ops->trim(device->context, lba, count);
    }
done:
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_block_diagnostics_record_end(device, MTFS_BLOCK_OPERATION_TRIM,
        (count > UINT32_MAX) ? UINT32_MAX : (uint32_t)count,
        result, NULL, NULL);
#endif
    return result;
}
