#include "mtfs_sentinel_lab_injector.h"

#include <stddef.h>
#include <string.h>

static uint32_t saturating_increment(uint32_t value)
{
    return value == UINT32_MAX ? UINT32_MAX : value + 1U;
}

static uint32_t next_random(mtfs_sentinel_lab_injector_t *injector)
{
    uint32_t value = injector->random_state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    injector->random_state = value;
    return value;
}

static int should_inject(mtfs_sentinel_lab_injector_t *injector,
    uint32_t operation)
{
    uint32_t random;
    if (injector->injection.kind == MTFS_SENTINEL_LAB_INJECTION_NONE ||
        (injector->injection.operation_mask & operation) == 0U) {
        return 0;
    }
    injector->eligible_count = saturating_increment(injector->eligible_count);
    random = next_random(injector);
    if ((random % UINT32_C(1000)) >= injector->injection.rate_permille) {
        return 0;
    }
    injector->injection_count =
        saturating_increment(injector->injection_count);
    if (injector->injection.kind == MTFS_SENTINEL_LAB_INJECTION_DELAY &&
        injector->config.delay != NULL && injector->injection.delay_us != 0U) {
        injector->config.delay(injector->config.delay_context,
            injector->injection.delay_us);
    }
    return 1;
}

static mtfs_error_t injected_result(mtfs_sentinel_lab_injector_t *injector,
    uint32_t operation)
{
    if (should_inject(injector, operation) &&
        injector->injection.kind == MTFS_SENTINEL_LAB_INJECTION_ERROR) {
        return injector->injection.error;
    }
    return MTFS_OK;
}

static mtfs_error_t lab_initialize(void *context)
{
    mtfs_sentinel_lab_injector_t *injector = context;
    return mtfs_block_initialize(injector->config.downstream);
}

static mtfs_error_t lab_status(void *context, mtfs_block_status_t *status)
{
    mtfs_sentinel_lab_injector_t *injector = context;
    return mtfs_block_status(injector->config.downstream, status);
}

static mtfs_error_t lab_read(void *context, void *buffer, mtfs_lba_t lba,
    uint32_t count)
{
    mtfs_sentinel_lab_injector_t *injector = context;
    mtfs_error_t result = injected_result(injector,
        MTFS_SENTINEL_LAB_INJECT_READ);
    return result == MTFS_OK ?
        mtfs_block_read(injector->config.downstream, buffer, lba, count) :
        result;
}

static mtfs_error_t lab_write(void *context, const void *buffer,
    mtfs_lba_t lba, uint32_t count)
{
    mtfs_sentinel_lab_injector_t *injector = context;
    mtfs_error_t result = injected_result(injector,
        MTFS_SENTINEL_LAB_INJECT_WRITE);
    return result == MTFS_OK ?
        mtfs_block_write(injector->config.downstream, buffer, lba, count) :
        result;
}

static mtfs_error_t lab_sync(void *context)
{
    mtfs_sentinel_lab_injector_t *injector = context;
    mtfs_error_t result = injected_result(injector,
        MTFS_SENTINEL_LAB_INJECT_SYNC);
    return result == MTFS_OK ?
        mtfs_block_sync(injector->config.downstream) : result;
}

static mtfs_error_t lab_geometry(void *context,
    mtfs_block_geometry_t *geometry)
{
    mtfs_sentinel_lab_injector_t *injector = context;
    return mtfs_block_get_geometry(injector->config.downstream, geometry);
}

static mtfs_error_t lab_trim(void *context, mtfs_lba_t lba, mtfs_lba_t count)
{
    mtfs_sentinel_lab_injector_t *injector = context;
    return mtfs_block_trim(injector->config.downstream, lba, count);
}

static const mtfs_block_device_ops_t lab_ops = {
    lab_initialize, lab_status, lab_read, lab_write, lab_sync, lab_geometry,
    lab_trim
};

mtfs_error_t mtfs_sentinel_lab_injector_init(
    mtfs_sentinel_lab_injector_t *injector,
    const mtfs_sentinel_lab_injector_config_t *config)
{
    if (injector == NULL || config == NULL ||
        !mtfs_block_device_is_valid(config->downstream)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    (void)memset(injector, 0, sizeof(*injector));
    injector->config = *config;
    injector->device.ops = &lab_ops;
    injector->device.context = injector;
    injector->device.capabilities = config->downstream->capabilities;
    injector->initialized = 1U;
    return MTFS_OK;
}

mtfs_block_device_t *mtfs_sentinel_lab_injector_block_device(
    mtfs_sentinel_lab_injector_t *injector)
{
    return injector != NULL && injector->initialized ?
        &injector->device : NULL;
}

mtfs_error_t mtfs_sentinel_lab_injector_enable(
    mtfs_sentinel_lab_injector_t *injector,
    const mtfs_sentinel_lab_injection_t *injection)
{
    if (injector == NULL || !injector->initialized || injection == NULL ||
        injection->kind == MTFS_SENTINEL_LAB_INJECTION_NONE ||
        injection->kind > MTFS_SENTINEL_LAB_INJECTION_ERROR ||
        injection->operation_mask == 0U ||
        (injection->operation_mask & ~MTFS_SENTINEL_LAB_INJECT_ALL) != 0U ||
        injection->rate_permille > UINT32_C(1000) ||
        injection->seed == 0U ||
        (injection->kind == MTFS_SENTINEL_LAB_INJECTION_DELAY &&
         (injection->delay_us == 0U || injector->config.delay == NULL)) ||
        (injection->kind == MTFS_SENTINEL_LAB_INJECTION_ERROR &&
         injection->error == MTFS_OK)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    injector->injection = *injection;
    injector->random_state = injection->seed;
    injector->eligible_count = 0U;
    injector->injection_count = 0U;
    return MTFS_OK;
}

void mtfs_sentinel_lab_injector_disable(
    mtfs_sentinel_lab_injector_t *injector)
{
    if (injector == NULL) return;
    (void)memset(&injector->injection, 0, sizeof(injector->injection));
    injector->random_state = 0U;
}

void mtfs_sentinel_lab_injector_reset_statistics(
    mtfs_sentinel_lab_injector_t *injector)
{
    if (injector == NULL) return;
    injector->eligible_count = 0U;
    injector->injection_count = 0U;
}

uint32_t mtfs_sentinel_lab_injector_injection_count(
    const mtfs_sentinel_lab_injector_t *injector)
{
    return injector == NULL ? 0U : injector->injection_count;
}

uint32_t mtfs_sentinel_lab_injector_eligible_count(
    const mtfs_sentinel_lab_injector_t *injector)
{
    return injector == NULL ? 0U : injector->eligible_count;
}
