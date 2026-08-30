#include "test_block_diagnostics.h"

#include <string.h>

#include "mtfs_block_diagnostics_internal.h"

typedef struct fake_block
{
    mtfs_error_t result;
    mtfs_block_status_t status;
    mtfs_block_geometry_t geometry;
} fake_block_t;

static mtfs_error_t fake_initialize(void *opaque)
{
    return ((fake_block_t *)opaque)->result;
}
static mtfs_error_t fake_status(void *opaque, mtfs_block_status_t *status)
{
    fake_block_t *fake = opaque;
    if (fake->result == MTFS_OK) *status = fake->status;
    return fake->result;
}
static mtfs_error_t fake_read(void *opaque, void *buffer,
    mtfs_lba_t lba, uint32_t count)
{
    (void)buffer; (void)lba; (void)count;
    return ((fake_block_t *)opaque)->result;
}
static mtfs_error_t fake_write(void *opaque, const void *buffer,
    mtfs_lba_t lba, uint32_t count)
{
    (void)buffer; (void)lba; (void)count;
    return ((fake_block_t *)opaque)->result;
}
static mtfs_error_t fake_sync(void *opaque)
{
    return ((fake_block_t *)opaque)->result;
}
static mtfs_error_t fake_geometry(void *opaque, mtfs_block_geometry_t *geometry)
{
    fake_block_t *fake = opaque;
    if (fake->result == MTFS_OK) *geometry = fake->geometry;
    return fake->result;
}
static mtfs_error_t fake_trim(void *opaque, mtfs_lba_t lba, mtfs_lba_t count)
{
    (void)lba; (void)count;
    return ((fake_block_t *)opaque)->result;
}

static const mtfs_block_device_ops_t fake_ops = {
    fake_initialize, fake_status, fake_read, fake_write, fake_sync,
    fake_geometry, fake_trim
};

static mtfs_error_t failing_diagnostics_lock(void *opaque)
{
    (void)opaque;
    return MTFS_ERROR_NOT_READY;
}

static void diagnostics_unlock(void *opaque)
{
    (void)opaque;
}

int test_block_diagnostics(mtfs_test_t *test)
{
    fake_block_t fake;
    mtfs_block_device_t device;
    mtfs_block_device_t unsupported;
    mtfs_block_diagnostics_state_t state;
    mtfs_block_diagnostics_t snapshot;
    unsigned char buffer[512];

    (void)memset(&fake, 0, sizeof(fake));
    (void)memset(&device, 0, sizeof(device));
    fake.status = MTFS_BLOCK_STATUS_INITIALIZED |
        MTFS_BLOCK_STATUS_MEDIA_PRESENT;
    fake.geometry.sector_size = 512U;
    fake.geometry.sector_count = 16U;
    fake.geometry.erase_block_size = 1U;
    device.ops = &fake_ops;
    device.context = &fake;
    device.capabilities = MTFS_BLOCK_CAPABILITY_TRIM;
    (void)mtfs_block_diagnostics_attach(&device, &state);
    unsupported = device;
    unsupported.capabilities &= ~MTFS_BLOCK_CAPABILITY_DIAGNOSTICS;
    unsupported.diagnostics = NULL;

    if (!MTFS_TEST_CHECK(test,
            mtfs_block_diagnostics_get(&unsupported, &snapshot) ==
                MTFS_ERROR_NOT_SUPPORTED,
            "custom device may omit diagnostics")) return 1;
    if (!MTFS_TEST_CHECK(test,
            mtfs_block_diagnostics_get(&device, &snapshot) == MTFS_OK &&
            snapshot.api_version == MTFS_BLOCK_DIAGNOSTICS_API_VERSION &&
            snapshot.struct_size == sizeof(snapshot) &&
            (snapshot.validity_mask & MTFS_BLOCK_DIAGNOSTICS_VALID_GEOMETRY) == 0U,
            "initial snapshot is versioned and marks uncached fields invalid")) return 1;

    (void)mtfs_block_initialize(&device);
    (void)mtfs_block_status(&device, &fake.status);
    (void)mtfs_block_get_geometry(&device, &fake.geometry);
    (void)mtfs_block_read(&device, buffer, 0U, 2U);
    (void)mtfs_block_write(&device, buffer, 2U, 3U);
    (void)mtfs_block_sync(&device);
    (void)mtfs_block_trim(&device, 5U, 1U);
    if (!MTFS_TEST_CHECK(test,
            mtfs_block_diagnostics_get(&device, &snapshot) == MTFS_OK &&
            snapshot.initialize_successes == 1U && snapshot.status_calls == 1U &&
            snapshot.geometry_calls == 1U && snapshot.read_successes == 1U &&
            snapshot.write_successes == 1U && snapshot.sync_successes == 1U &&
            snapshot.trim_successes == 1U &&
            snapshot.read_sectors_requested == 2U &&
            snapshot.read_sectors_completed == 2U &&
            snapshot.write_sectors_requested == 3U &&
            snapshot.write_sectors_completed == 3U &&
            (snapshot.validity_mask & MTFS_BLOCK_DIAGNOSTICS_VALID_STATUS) != 0U &&
            (snapshot.validity_mask & MTFS_BLOCK_DIAGNOSTICS_VALID_GEOMETRY) != 0U,
            "successful operations populate state and counters")) return 1;

    fake.result = MTFS_ERROR_IO;
    (void)mtfs_block_read(&device, buffer, 0U, 1U);
    fake.result = MTFS_ERROR_NOT_READY;
    (void)mtfs_block_sync(&device);
    fake.result = MTFS_ERROR_NO_MEDIA;
    (void)mtfs_block_initialize(&device);
    fake.result = MTFS_ERROR_WRITE_PROTECTED;
    (void)mtfs_block_write(&device, buffer, 0U, 1U);
    fake.result = MTFS_ERROR_NOT_READY;
    (void)mtfs_block_status(&device, &fake.status);
    fake.result = MTFS_ERROR_IO;
    (void)mtfs_block_get_geometry(&device, &fake.geometry);
    fake.result = MTFS_ERROR_NOT_SUPPORTED;
    (void)mtfs_block_trim(&device, 0U, 1U);
    fake.result = MTFS_OK;
    (void)mtfs_block_read(&device, buffer, 16U, 1U);
    if (!MTFS_TEST_CHECK(test,
            mtfs_block_diagnostics_get(&device, &snapshot) == MTFS_OK &&
            snapshot.initialize_failures == 1U &&
            snapshot.status_failures == 1U && snapshot.read_failures == 2U &&
            snapshot.write_failures == 1U && snapshot.sync_failures == 1U &&
            snapshot.geometry_failures == 1U && snapshot.trim_failures == 1U &&
            snapshot.read_sectors_requested == 4U &&
            snapshot.read_sectors_completed == 2U &&
            snapshot.write_sectors_requested == 4U &&
            snapshot.write_sectors_completed == 3U &&
            snapshot.io_errors == 2U && snapshot.not_ready_errors == 2U &&
            snapshot.no_media_errors == 1U &&
            snapshot.write_protected_errors == 1U &&
            snapshot.out_of_range_errors == 1U &&
            snapshot.other_errors == 1U &&
            snapshot.last_operation == MTFS_BLOCK_OPERATION_READ &&
            snapshot.last_error == MTFS_ERROR_OUT_OF_RANGE,
            "all operation failures are classified without claiming completion")) return 1;

    state.snapshot.read_calls = UINT32_MAX;
    (void)mtfs_block_read(&device, buffer, 0U, 1U);
    if (!MTFS_TEST_CHECK(test,
            mtfs_block_diagnostics_get(&device, &snapshot) == MTFS_OK &&
            snapshot.read_calls == UINT32_MAX,
            "32-bit counters saturate instead of wrapping")) return 1;
    if (!MTFS_TEST_CHECK(test,
            mtfs_block_diagnostics_reset(&device) == MTFS_OK &&
            mtfs_block_diagnostics_get(&device, &snapshot) == MTFS_OK &&
            snapshot.reset_epoch == 1U && snapshot.read_calls == 0U &&
            snapshot.sector_count == 16U &&
            snapshot.last_error == MTFS_OK,
            "reset clears counters and preserves last-known state")) return 1;

    state.sequence = 1U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_block_diagnostics_get(&device, &snapshot) ==
                MTFS_ERROR_NOT_READY,
            "snapshot reports a simulated update collision")) return 1;
    state.sequence = 2U;
#if MTFS_ENABLE_STORAGE_SENTINEL
    fake.result = MTFS_OK;
    if (!MTFS_TEST_CHECK(test,
            mtfs_block_diagnostics_attach_locked(&device, &state,
                failing_diagnostics_lock, diagnostics_unlock, NULL) ==
                MTFS_OK &&
            mtfs_block_sync(&device) == MTFS_OK,
            "ordinary diagnostics lock failure does not change I/O result"))
        return 1;
#endif
    return 0;
}
