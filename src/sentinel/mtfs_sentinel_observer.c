#include "mtfs_sentinel_observer.h"

#if MTFS_ENABLE_STORAGE_SENTINEL

#include <stddef.h>
#include <string.h>

const uint64_t mtfs_sentinel_histogram_upper_us[
    MTFS_SENTINEL_HISTOGRAM_BUCKETS - 1U] = {
    UINT64_C(1), UINT64_C(2), UINT64_C(4), UINT64_C(8),
    UINT64_C(16), UINT64_C(32), UINT64_C(64), UINT64_C(128),
    UINT64_C(256), UINT64_C(512), UINT64_C(1024), UINT64_C(2048),
    UINT64_C(4096), UINT64_C(8192), UINT64_C(16384), UINT64_C(32768),
    UINT64_C(65536), UINT64_C(262144), UINT64_C(1048576),
    UINT64_C(4194304), UINT64_C(16777216)
};

static mtfs_error_t observer_initialize(void *opaque);
static mtfs_error_t observer_status(void *opaque, mtfs_block_status_t *status);
static mtfs_error_t observer_read(
    void *opaque, void *buffer, mtfs_lba_t lba, uint32_t count);
static mtfs_error_t observer_write(
    void *opaque, const void *buffer, mtfs_lba_t lba, uint32_t count);
static mtfs_error_t observer_sync(void *opaque);
static mtfs_error_t observer_geometry(
    void *opaque, mtfs_block_geometry_t *geometry);
static mtfs_error_t observer_trim(
    void *opaque, mtfs_lba_t lba, mtfs_lba_t count);

static const mtfs_block_device_ops_t observer_ops = {
    observer_initialize, observer_status, observer_read, observer_write,
    observer_sync, observer_geometry, observer_trim
};
static const mtfs_block_device_ops_t observer_ops_no_trim = {
    observer_initialize, observer_status, observer_read, observer_write,
    observer_sync, observer_geometry, NULL
};

uint32_t mtfs_sentinel_histogram_bucket(uint64_t elapsed_us)
{
    uint32_t bucket;
    for (bucket = 0U; bucket < MTFS_SENTINEL_HISTOGRAM_BUCKETS - 1U;
         ++bucket) {
        if (elapsed_us <= mtfs_sentinel_histogram_upper_us[bucket]) {
            break;
        }
    }
    return bucket;
}

static uint64_t saturating_add(uint64_t a, uint64_t b, uint32_t *flags)
{
    if (b > UINT64_MAX - a) {
        *flags |= MTFS_SENTINEL_OBSERVER_FLAG_SATURATED;
        return UINT64_MAX;
    }
    return a + b;
}

static mtfs_error_t enter_call(mtfs_sentinel_observer_t *observer)
{
    mtfs_error_t result = observer->config.lock(observer->config.lock_context);
    if (result != MTFS_OK) return result;
    if (!observer->initialized || observer->active_calls == UINT32_MAX) {
        observer->config.unlock(observer->config.lock_context);
        return MTFS_ERROR_INVALID_STATE;
    }
    ++observer->active_calls;
    observer->config.unlock(observer->config.lock_context);
    return MTFS_OK;
}

static void leave_call(mtfs_sentinel_observer_t *observer)
{
    if (observer->config.lock(observer->config.lock_context) == MTFS_OK) {
        if (observer->active_calls != 0U) --observer->active_calls;
        observer->config.unlock(observer->config.lock_context);
    }
}

static void record_timing(mtfs_sentinel_observer_t *observer,
    mtfs_sentinel_timing_t *timing, int start_valid, uint64_t start_us,
    int end_valid, uint64_t end_us)
{
    uint64_t elapsed;
    uint32_t bucket;
    if (observer->config.lock(observer->config.lock_context) != MTFS_OK) return;
    if (!start_valid || !end_valid || end_us < start_us) {
        timing->invalid_samples = saturating_add(
            timing->invalid_samples, UINT64_C(1), &observer->snapshot.flags);
    } else {
        elapsed = end_us - start_us;
        bucket = mtfs_sentinel_histogram_bucket(elapsed);
        timing->sample_count = saturating_add(
            timing->sample_count, UINT64_C(1), &observer->snapshot.flags);
        timing->total_latency_us = saturating_add(
            timing->total_latency_us, elapsed, &observer->snapshot.flags);
        timing->histogram[bucket] = saturating_add(
            timing->histogram[bucket], UINT64_C(1), &observer->snapshot.flags);
    }
    observer->config.unlock(observer->config.lock_context);
}

typedef mtfs_error_t (*timed_call_fn)(mtfs_sentinel_observer_t *, void *);

static mtfs_error_t timed_call(mtfs_sentinel_observer_t *observer,
    mtfs_sentinel_timing_t *timing, timed_call_fn call, void *argument)
{
    uint64_t start_us = 0U, end_us = 0U;
    int start_valid, end_valid;
    mtfs_error_t result;
    result = enter_call(observer);
    if (result != MTFS_OK) return result;
    start_valid = observer->config.clock(observer->config.clock_context,
        &start_us) == MTFS_OK;
    result = call(observer, argument);
    end_valid = observer->config.clock(observer->config.clock_context,
        &end_us) == MTFS_OK;
    record_timing(observer, timing, start_valid, start_us, end_valid, end_us);
    leave_call(observer);
    return result;
}

static mtfs_error_t observer_initialize(void *opaque)
{
    mtfs_sentinel_observer_t *o = opaque;
    mtfs_error_t result = enter_call(o);
    if (result != MTFS_OK) return result;
    result = o->config.downstream->ops->initialize(o->config.downstream->context);
    leave_call(o);
    return result;
}
static mtfs_error_t observer_status(void *opaque, mtfs_block_status_t *status)
{
    mtfs_sentinel_observer_t *o = opaque;
    mtfs_error_t result = enter_call(o);
    if (result != MTFS_OK) return result;
    result = o->config.downstream->ops->status(o->config.downstream->context, status);
    leave_call(o);
    return result;
}
static mtfs_error_t observer_geometry(void *opaque, mtfs_block_geometry_t *geometry)
{
    mtfs_sentinel_observer_t *o = opaque;
    mtfs_error_t result = enter_call(o);
    if (result != MTFS_OK) return result;
    result = o->config.downstream->ops->get_geometry(
        o->config.downstream->context, geometry);
    leave_call(o);
    return result;
}
static mtfs_error_t observer_trim(void *opaque, mtfs_lba_t lba, mtfs_lba_t count)
{
    mtfs_sentinel_observer_t *o = opaque;
    mtfs_error_t result = enter_call(o);
    if (result != MTFS_OK) return result;
    result = o->config.downstream->ops->trim(
        o->config.downstream->context, lba, count);
    leave_call(o);
    return result;
}

typedef struct io_args { void *buffer; mtfs_lba_t lba; uint32_t count; } io_args_t;
static mtfs_error_t call_read(mtfs_sentinel_observer_t *o, void *opaque)
{
    io_args_t *a = opaque;
    return o->config.downstream->ops->read(
        o->config.downstream->context, a->buffer, a->lba, a->count);
}
static mtfs_error_t call_write(mtfs_sentinel_observer_t *o, void *opaque)
{
    io_args_t *a = opaque;
    return o->config.downstream->ops->write(
        o->config.downstream->context, a->buffer, a->lba, a->count);
}
static mtfs_error_t call_sync(mtfs_sentinel_observer_t *o, void *opaque)
{
    (void)opaque;
    return o->config.downstream->ops->sync(o->config.downstream->context);
}
static mtfs_error_t observer_read(
    void *opaque, void *buffer, mtfs_lba_t lba, uint32_t count)
{
    mtfs_sentinel_observer_t *o = opaque;
    io_args_t args = {buffer, lba, count};
    return timed_call(o, &o->snapshot.read, call_read, &args);
}
static mtfs_error_t observer_write(
    void *opaque, const void *buffer, mtfs_lba_t lba, uint32_t count)
{
    mtfs_sentinel_observer_t *o = opaque;
    io_args_t args = {(void *)buffer, lba, count};
    return timed_call(o, &o->snapshot.write, call_write, &args);
}
static mtfs_error_t observer_sync(void *opaque)
{
    mtfs_sentinel_observer_t *o = opaque;
    return timed_call(o, &o->snapshot.sync, call_sync, NULL);
}

mtfs_error_t mtfs_sentinel_observer_init(
    mtfs_sentinel_observer_t *observer,
    const mtfs_sentinel_observer_config_t *config)
{
    mtfs_error_t result;
    if ((observer == NULL) || (config == NULL) ||
        !mtfs_block_device_is_valid(config->downstream) ||
        (config->clock == NULL) || (config->lock == NULL) ||
        (config->unlock == NULL)) return MTFS_ERROR_INVALID_ARGUMENT;
    (void)memset(observer, 0, sizeof(*observer));
    observer->config = *config;
    observer->block_device.ops =
        ((config->downstream->capabilities & MTFS_BLOCK_CAPABILITY_TRIM) != 0U) ?
        &observer_ops : &observer_ops_no_trim;
    observer->block_device.context = observer;
    observer->block_device.capabilities = config->downstream->capabilities &
        ~MTFS_BLOCK_CAPABILITY_DIAGNOSTICS;
    observer->snapshot.api_version = MTFS_SENTINEL_OBSERVER_API_VERSION;
    observer->snapshot.struct_size = (uint16_t)sizeof(observer->snapshot);
    observer->initialized = 1U;
    result = mtfs_block_diagnostics_attach_locked(
        &observer->block_device, &observer->diagnostics,
        config->lock, config->unlock, config->lock_context);
    if (result != MTFS_OK) observer->initialized = 0U;
    return result;
}

mtfs_error_t mtfs_sentinel_observer_deinit(mtfs_sentinel_observer_t *observer)
{
    mtfs_error_t result;
    if (observer == NULL || !observer->initialized) return MTFS_ERROR_INVALID_ARGUMENT;
    result = observer->config.lock(observer->config.lock_context);
    if (result != MTFS_OK) return result;
    if (observer->active_calls != 0U) {
        observer->config.unlock(observer->config.lock_context);
        return MTFS_ERROR_NOT_READY;
    }
    observer->initialized = 0U;
    observer->config.unlock(observer->config.lock_context);
    return MTFS_OK;
}

mtfs_block_device_t *mtfs_sentinel_observer_block_device(
    mtfs_sentinel_observer_t *observer)
{
    return (observer != NULL && observer->initialized) ?
        &observer->block_device : NULL;
}

mtfs_error_t mtfs_sentinel_observer_get(
    mtfs_sentinel_observer_t *observer,
    mtfs_sentinel_observer_snapshot_t *snapshot)
{
    mtfs_error_t result;
    if (observer == NULL || snapshot == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    result = observer->config.lock(observer->config.lock_context);
    if (result != MTFS_OK) return result;
    if (!observer->initialized) result = MTFS_ERROR_INVALID_STATE;
    else *snapshot = observer->snapshot;
    observer->config.unlock(observer->config.lock_context);
    return result;
}

mtfs_error_t mtfs_sentinel_observer_reset(mtfs_sentinel_observer_t *observer)
{
    uint32_t epoch;
    mtfs_error_t result;
    if (observer == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    result = observer->config.lock(observer->config.lock_context);
    if (result != MTFS_OK) return result;
    if (!observer->initialized || observer->active_calls != 0U) {
        observer->config.unlock(observer->config.lock_context);
        return MTFS_ERROR_NOT_READY;
    }
    epoch = observer->snapshot.reset_epoch + 1U;
    (void)memset(&observer->snapshot, 0, sizeof(observer->snapshot));
    observer->snapshot.api_version = MTFS_SENTINEL_OBSERVER_API_VERSION;
    observer->snapshot.struct_size = (uint16_t)sizeof(observer->snapshot);
    observer->snapshot.reset_epoch = epoch;
    observer->config.unlock(observer->config.lock_context);
    return MTFS_OK;
}

#endif /* MTFS_ENABLE_STORAGE_SENTINEL */
