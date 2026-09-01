#include <stdint.h>
#include <string.h>

#include "mtfs_sentinel_lab_injector.h"

typedef struct fake_device
{
    mtfs_block_device_t device;
    uint32_t reads;
    uint32_t writes;
    uint32_t syncs;
    void *last_buffer;
    mtfs_lba_t last_lba;
} fake_device_t;

static mtfs_error_t fake_init(void *context) { (void)context; return MTFS_OK; }
static mtfs_error_t fake_status(void *context, mtfs_block_status_t *status)
{
    (void)context; *status = MTFS_BLOCK_STATUS_INITIALIZED; return MTFS_OK;
}
static mtfs_error_t fake_read(void *context, void *buffer, mtfs_lba_t lba,
    uint32_t count)
{
    fake_device_t *fake = context; (void)count;
    ++fake->reads; fake->last_buffer = buffer; fake->last_lba = lba;
    return MTFS_OK;
}
static mtfs_error_t fake_write(void *context, const void *buffer,
    mtfs_lba_t lba, uint32_t count)
{
    fake_device_t *fake = context; (void)count;
    ++fake->writes; fake->last_buffer = (void *)buffer; fake->last_lba = lba;
    return MTFS_OK;
}
static mtfs_error_t fake_sync(void *context)
{
    ++((fake_device_t *)context)->syncs; return MTFS_OK;
}
static mtfs_error_t fake_geometry(void *context, mtfs_block_geometry_t *geometry)
{
    (void)context; geometry->sector_size = 512U; geometry->sector_count = 1024U;
    geometry->erase_block_size = 1U; return MTFS_OK;
}
static const mtfs_block_device_ops_t fake_ops = {
    fake_init, fake_status, fake_read, fake_write, fake_sync, fake_geometry, NULL
};

static uint32_t delay_calls;
static uint32_t last_delay;
static void delay(void *context, uint32_t delay_us)
{
    (void)context; ++delay_calls; last_delay = delay_us;
}

int main(void)
{
    fake_device_t fake;
    mtfs_sentinel_lab_injector_t injector;
    mtfs_sentinel_lab_injector_config_t config;
    mtfs_sentinel_lab_injection_t injection;
    mtfs_block_device_t *device;
    uint8_t buffer[512];
    uint32_t first_count;
    uint32_t index;
    (void)memset(&fake, 0, sizeof(fake));
    fake.device.ops = &fake_ops;
    fake.device.context = &fake;
    config.downstream = &fake.device;
    config.delay = delay;
    config.delay_context = NULL;
    if (mtfs_sentinel_lab_injector_init(&injector, &config) != MTFS_OK)
        return 1;
    device = mtfs_sentinel_lab_injector_block_device(&injector);
    if (mtfs_block_read(device, buffer, 7U, 1U) != MTFS_OK ||
        fake.reads != 1U || fake.last_buffer != buffer || fake.last_lba != 7U ||
        delay_calls != 0U) return 1;
    (void)memset(&injection, 0, sizeof(injection));
    injection.kind = MTFS_SENTINEL_LAB_INJECTION_DELAY;
    injection.operation_mask = MTFS_SENTINEL_LAB_INJECT_READ;
    injection.rate_permille = 500U;
    injection.delay_us = 1234U;
    injection.seed = 99U;
    if (mtfs_sentinel_lab_injector_enable(&injector, &injection) != MTFS_OK)
        return 1;
    for (index = 0U; index < 20U; ++index)
        if (mtfs_block_read(device, buffer, 7U, 1U) != MTFS_OK) return 1;
    first_count = mtfs_sentinel_lab_injector_injection_count(&injector);
    if (first_count == 0U || first_count >= 20U || last_delay != 1234U)
        return 1;
    if (mtfs_sentinel_lab_injector_enable(&injector, &injection) != MTFS_OK)
        return 1;
    delay_calls = 0U;
    for (index = 0U; index < 20U; ++index) {
        if (mtfs_block_read(device, buffer, 7U, 1U) != MTFS_OK) return 1;
    }
    if (mtfs_sentinel_lab_injector_injection_count(&injector) != first_count ||
        delay_calls != first_count)
        return 1;
    injection.kind = MTFS_SENTINEL_LAB_INJECTION_ERROR;
    injection.operation_mask = MTFS_SENTINEL_LAB_INJECT_SYNC;
    injection.rate_permille = 1000U;
    injection.error = MTFS_ERROR_IO;
    injection.seed = 1U;
    if (mtfs_sentinel_lab_injector_enable(&injector, &injection) != MTFS_OK ||
        mtfs_block_sync(device) != MTFS_ERROR_IO || fake.syncs != 0U)
        return 1;
    mtfs_sentinel_lab_injector_disable(&injector);
    if (mtfs_block_sync(device) != MTFS_OK || fake.syncs != 1U)
        return 1;
    injection.seed = 0U;
    if (mtfs_sentinel_lab_injector_enable(&injector, &injection) !=
        MTFS_ERROR_INVALID_ARGUMENT) return 1;
    return 0;
}
