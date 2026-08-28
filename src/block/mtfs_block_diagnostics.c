#include "mtfs_block_diagnostics_internal.h"

#include <stddef.h>
#include <string.h>

#if MTFS_ENABLE_DIAGNOSTICS
static void mtfs_diagnostics_increment(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++*value;
    }
}

static void mtfs_diagnostics_add(uint32_t *value, uint32_t amount)
{
    if (amount > (UINT32_MAX - *value)) {
        *value = UINT32_MAX;
    } else {
        *value += amount;
    }
}

static void mtfs_diagnostics_classify_error(
    mtfs_block_diagnostics_t *diagnostics, mtfs_error_t result)
{
    switch (result) {
    case MTFS_ERROR_IO:
        mtfs_diagnostics_increment(&diagnostics->io_errors);
        break;
    case MTFS_ERROR_NOT_READY:
        mtfs_diagnostics_increment(&diagnostics->not_ready_errors);
        break;
    case MTFS_ERROR_NO_MEDIA:
        mtfs_diagnostics_increment(&diagnostics->no_media_errors);
        break;
    case MTFS_ERROR_WRITE_PROTECTED:
        mtfs_diagnostics_increment(&diagnostics->write_protected_errors);
        break;
    case MTFS_ERROR_OUT_OF_RANGE:
        mtfs_diagnostics_increment(&diagnostics->out_of_range_errors);
        break;
    default:
        mtfs_diagnostics_increment(&diagnostics->other_errors);
        break;
    }
}

mtfs_error_t mtfs_block_diagnostics_attach(
    mtfs_block_device_t *device, mtfs_block_diagnostics_state_t *state)
{
#if MTFS_ENABLE_STORAGE_SENTINEL
    return mtfs_block_diagnostics_attach_locked(
        device, state, NULL, NULL, NULL);
#else
    if ((device == NULL) || (state == NULL)) return MTFS_ERROR_INVALID_ARGUMENT;
    (void)memset(state, 0, sizeof(*state));
    state->snapshot.api_version = MTFS_BLOCK_DIAGNOSTICS_API_VERSION;
    state->snapshot.struct_size = (uint16_t)sizeof(state->snapshot);
    state->snapshot.validity_mask =
        MTFS_BLOCK_DIAGNOSTICS_VALID_READ_COMPLETED |
        MTFS_BLOCK_DIAGNOSTICS_VALID_WRITE_COMPLETED;
    state->snapshot.flags = MTFS_BLOCK_DIAGNOSTICS_FLAG_COUNTERS_SATURATE;
    device->capabilities |= MTFS_BLOCK_CAPABILITY_DIAGNOSTICS;
    state->snapshot.capabilities = device->capabilities;
    device->diagnostics = state;
    return MTFS_OK;
#endif
}

#if MTFS_ENABLE_STORAGE_SENTINEL
mtfs_error_t mtfs_block_diagnostics_attach_locked(
    mtfs_block_device_t *device, mtfs_block_diagnostics_state_t *state,
    mtfs_error_t (*lock)(void *context), void (*unlock)(void *context),
    void *lock_context)
{
    if ((device == NULL) || (state == NULL) || ((lock == NULL) != (unlock == NULL))) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    (void)memset(state, 0, sizeof(*state));
    state->snapshot.api_version = MTFS_BLOCK_DIAGNOSTICS_API_VERSION;
    state->snapshot.struct_size = (uint16_t)sizeof(state->snapshot);
    state->snapshot.validity_mask =
        MTFS_BLOCK_DIAGNOSTICS_VALID_READ_COMPLETED |
        MTFS_BLOCK_DIAGNOSTICS_VALID_WRITE_COMPLETED;
    state->snapshot.flags = MTFS_BLOCK_DIAGNOSTICS_FLAG_COUNTERS_SATURATE;
    device->capabilities |= MTFS_BLOCK_CAPABILITY_DIAGNOSTICS;
    state->snapshot.capabilities = device->capabilities;
    device->diagnostics = state;
    state->lock = lock;
    state->unlock = unlock;
    state->lock_context = lock_context;
    return MTFS_OK;
}
#endif

void mtfs_block_diagnostics_record_begin(
    mtfs_block_device_t *device, mtfs_block_operation_t operation,
    uint32_t sectors)
{
    mtfs_block_diagnostics_t *d;
    if ((device == NULL) ||
        ((device->capabilities & MTFS_BLOCK_CAPABILITY_DIAGNOSTICS) == 0U) ||
        (device->diagnostics == NULL)) {
        return;
    }
#if MTFS_ENABLE_STORAGE_SENTINEL
    if (device->diagnostics->lock != NULL &&
        device->diagnostics->lock(device->diagnostics->lock_context) != MTFS_OK)
        return;
#endif
    ++device->diagnostics->sequence;
    d = &device->diagnostics->snapshot;
    d->last_operation = (uint32_t)operation;
    switch (operation) {
    case MTFS_BLOCK_OPERATION_INITIALIZE: mtfs_diagnostics_increment(&d->initialize_calls); break;
    case MTFS_BLOCK_OPERATION_STATUS: mtfs_diagnostics_increment(&d->status_calls); break;
    case MTFS_BLOCK_OPERATION_READ:
        mtfs_diagnostics_increment(&d->read_calls);
        mtfs_diagnostics_add(&d->read_sectors_requested, sectors);
        break;
    case MTFS_BLOCK_OPERATION_WRITE:
        mtfs_diagnostics_increment(&d->write_calls);
        mtfs_diagnostics_add(&d->write_sectors_requested, sectors);
        break;
    case MTFS_BLOCK_OPERATION_SYNC: mtfs_diagnostics_increment(&d->sync_calls); break;
    case MTFS_BLOCK_OPERATION_GET_GEOMETRY: mtfs_diagnostics_increment(&d->geometry_calls); break;
    case MTFS_BLOCK_OPERATION_TRIM: mtfs_diagnostics_increment(&d->trim_calls); break;
    default: break;
    }
    ++device->diagnostics->sequence;
#if MTFS_ENABLE_STORAGE_SENTINEL
    if (device->diagnostics->unlock != NULL)
        device->diagnostics->unlock(device->diagnostics->lock_context);
#endif
}

void mtfs_block_diagnostics_record_end(
    mtfs_block_device_t *device, mtfs_block_operation_t operation,
    uint32_t sectors, mtfs_error_t result, const mtfs_block_status_t *status,
    const mtfs_block_geometry_t *geometry)
{
    mtfs_block_diagnostics_t *d;
    if ((device == NULL) ||
        ((device->capabilities & MTFS_BLOCK_CAPABILITY_DIAGNOSTICS) == 0U) ||
        (device->diagnostics == NULL)) {
        return;
    }
#if MTFS_ENABLE_STORAGE_SENTINEL
    if (device->diagnostics->lock != NULL &&
        device->diagnostics->lock(device->diagnostics->lock_context) != MTFS_OK)
        return;
#endif
    ++device->diagnostics->sequence;
    d = &device->diagnostics->snapshot;
    d->last_error = (int32_t)result;
    if ((result == MTFS_OK) && (status != NULL)) {
        d->status = *status;
        d->validity_mask |= MTFS_BLOCK_DIAGNOSTICS_VALID_STATUS;
    }
    if ((result == MTFS_OK) && (geometry != NULL)) {
        d->sector_size = geometry->sector_size;
        d->sector_count = geometry->sector_count;
        d->erase_block_size = geometry->erase_block_size;
        d->validity_mask |= MTFS_BLOCK_DIAGNOSTICS_VALID_GEOMETRY;
    }
    switch (operation) {
    case MTFS_BLOCK_OPERATION_INITIALIZE:
        mtfs_diagnostics_increment((result == MTFS_OK) ? &d->initialize_successes : &d->initialize_failures); break;
    case MTFS_BLOCK_OPERATION_STATUS:
        if (result != MTFS_OK) {
            mtfs_diagnostics_increment(&d->status_failures);
        }
        break;
    case MTFS_BLOCK_OPERATION_READ:
        mtfs_diagnostics_increment((result == MTFS_OK) ? &d->read_successes : &d->read_failures);
        if (result == MTFS_OK) mtfs_diagnostics_add(&d->read_sectors_completed, sectors);
        break;
    case MTFS_BLOCK_OPERATION_WRITE:
        mtfs_diagnostics_increment((result == MTFS_OK) ? &d->write_successes : &d->write_failures);
        if (result == MTFS_OK) mtfs_diagnostics_add(&d->write_sectors_completed, sectors);
        break;
    case MTFS_BLOCK_OPERATION_SYNC:
        mtfs_diagnostics_increment((result == MTFS_OK) ? &d->sync_successes : &d->sync_failures); break;
    case MTFS_BLOCK_OPERATION_GET_GEOMETRY:
        if (result != MTFS_OK) {
            mtfs_diagnostics_increment(&d->geometry_failures);
        }
        break;
    case MTFS_BLOCK_OPERATION_TRIM:
        mtfs_diagnostics_increment((result == MTFS_OK) ? &d->trim_successes : &d->trim_failures); break;
    default: break;
    }
    if (result != MTFS_OK) {
        mtfs_diagnostics_classify_error(d, result);
    }
    ++device->diagnostics->sequence;
#if MTFS_ENABLE_STORAGE_SENTINEL
    if (device->diagnostics->unlock != NULL)
        device->diagnostics->unlock(device->diagnostics->lock_context);
#endif
}

mtfs_error_t mtfs_block_diagnostics_get(
    mtfs_block_device_t *device, mtfs_block_diagnostics_t *snapshot)
{
    uint32_t before;
    uint32_t after;
    unsigned int attempts;
    if ((device == NULL) || (snapshot == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    if (((device->capabilities & MTFS_BLOCK_CAPABILITY_DIAGNOSTICS) == 0U) ||
        (device->diagnostics == NULL)) {
        return MTFS_ERROR_NOT_SUPPORTED;
    }
#if MTFS_ENABLE_STORAGE_SENTINEL
    if (device->diagnostics->lock != NULL) {
        mtfs_error_t result =
            device->diagnostics->lock(device->diagnostics->lock_context);
        if (result != MTFS_OK) return result;
        *snapshot = device->diagnostics->snapshot;
        device->diagnostics->unlock(device->diagnostics->lock_context);
        return MTFS_OK;
    }
#endif
    for (attempts = 0U; attempts < 8U; ++attempts) {
        before = device->diagnostics->sequence;
        if ((before & 1U) != 0U) continue;
        *snapshot = device->diagnostics->snapshot;
        after = device->diagnostics->sequence;
        if ((before == after) && ((after & 1U) == 0U)) return MTFS_OK;
    }
    return MTFS_ERROR_NOT_READY;
}

mtfs_error_t mtfs_block_diagnostics_reset(mtfs_block_device_t *device)
{
    mtfs_block_diagnostics_t preserved;
    uint32_t epoch;
    if (device == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    if (((device->capabilities & MTFS_BLOCK_CAPABILITY_DIAGNOSTICS) == 0U) ||
        (device->diagnostics == NULL)) return MTFS_ERROR_NOT_SUPPORTED;
#if MTFS_ENABLE_STORAGE_SENTINEL
    if (device->diagnostics->lock != NULL) {
        mtfs_error_t result =
            device->diagnostics->lock(device->diagnostics->lock_context);
        if (result != MTFS_OK) return result;
    }
#endif
    ++device->diagnostics->sequence;
    preserved = device->diagnostics->snapshot;
    epoch = preserved.reset_epoch + 1U;
    (void)memset(&device->diagnostics->snapshot, 0,
        sizeof(device->diagnostics->snapshot));
    device->diagnostics->snapshot.api_version = MTFS_BLOCK_DIAGNOSTICS_API_VERSION;
    device->diagnostics->snapshot.struct_size =
        (uint16_t)sizeof(device->diagnostics->snapshot);
    device->diagnostics->snapshot.validity_mask = preserved.validity_mask;
    device->diagnostics->snapshot.reset_epoch = epoch;
    device->diagnostics->snapshot.flags = preserved.flags;
    device->diagnostics->snapshot.capabilities = preserved.capabilities;
    device->diagnostics->snapshot.status = preserved.status;
    device->diagnostics->snapshot.sector_size = preserved.sector_size;
    device->diagnostics->snapshot.sector_count = preserved.sector_count;
    device->diagnostics->snapshot.erase_block_size = preserved.erase_block_size;
    device->diagnostics->snapshot.last_operation = preserved.last_operation;
    device->diagnostics->snapshot.last_error = preserved.last_error;
    ++device->diagnostics->sequence;
#if MTFS_ENABLE_STORAGE_SENTINEL
    if (device->diagnostics->unlock != NULL)
        device->diagnostics->unlock(device->diagnostics->lock_context);
#endif
    return MTFS_OK;
}
#else
mtfs_error_t mtfs_block_diagnostics_attach(
    mtfs_block_device_t *device, mtfs_block_diagnostics_state_t *state)
{
    if ((device == NULL) || (state == NULL)) return MTFS_ERROR_INVALID_ARGUMENT;
    return MTFS_ERROR_NOT_SUPPORTED;
}

mtfs_error_t mtfs_block_diagnostics_get(
    mtfs_block_device_t *device, mtfs_block_diagnostics_t *snapshot)
{
    if ((device == NULL) || (snapshot == NULL)) return MTFS_ERROR_INVALID_ARGUMENT;
    return MTFS_ERROR_NOT_SUPPORTED;
}

mtfs_error_t mtfs_block_diagnostics_reset(mtfs_block_device_t *device)
{
    return (device == NULL) ? MTFS_ERROR_INVALID_ARGUMENT : MTFS_ERROR_NOT_SUPPORTED;
}
#endif
