#include "mtfs_sentinel.h"

#if MTFS_ENABLE_STORAGE_SENTINEL

#include <stddef.h>
#include <string.h>

static uint64_t add64(uint64_t a, uint64_t b, uint32_t *flags)
{
    if (b > UINT64_MAX - a) {
        *flags |= MTFS_SENTINEL_FLAG_COUNTER_SATURATED;
        return UINT64_MAX;
    }
    return a + b;
}

static int delta64(uint64_t current, uint64_t previous, uint64_t *delta)
{
    if (current < previous) return 0;
    *delta = current - previous;
    return 1;
}

static int delta32(uint32_t current, uint32_t previous, uint64_t *delta)
{
    return delta64(current, previous, delta);
}

static int media_event_delta(
    const mtfs_sentinel_sample_metadata_t *current,
    const mtfs_sentinel_sample_metadata_t *previous,
    mtfs_sentinel_feature_v1_t *feature)
{
    uint64_t inserted;
    uint64_t removed;
    uint64_t errors;
    if (!delta32(current->inserted_events, previous->inserted_events,
            &inserted) ||
        !delta32(current->removed_events, previous->removed_events,
            &removed) ||
        !delta32(current->error_events, previous->error_events, &errors))
        return 0;
    feature->inserted_events = (uint32_t)inserted;
    feature->removed_events = (uint32_t)removed;
    feature->media_error_events = (uint32_t)errors;
    return 1;
}

/* Avoid target runtime-library dependencies for 64-bit division. */
static uint64_t divide64(uint64_t numerator, uint64_t denominator)
{
    uint64_t quotient = 0U;
    uint32_t bit;
    if (denominator == 0U) return UINT64_MAX;
    for (bit = 64U; bit != 0U; --bit) {
        uint32_t shift = bit - 1U;
        if (denominator <= (UINT64_MAX >> shift)) {
            uint64_t shifted = denominator << shift;
            if (shifted <= numerator) {
                numerator -= shifted;
                quotient |= UINT64_C(1) << shift;
            }
        }
    }
    return quotient;
}

static int multiply64(uint64_t left, uint64_t right, uint64_t *product)
{
    uint64_t result = 0U;
    while (right != 0U) {
        if ((right & UINT64_C(1)) != 0U) {
            if (left > UINT64_MAX - result) return 0;
            result += left;
        }
        right >>= 1U;
        if (right != 0U) {
            if (left > UINT64_MAX - left) return 0;
            left += left;
        }
    }
    *product = result;
    return 1;
}

static uint32_t ratio_q16(uint64_t numerator, uint64_t denominator)
{
    if (denominator == 0U) return 0U;
    if (numerator > (UINT64_MAX >> 16U)) return UINT32_MAX;
    numerator = divide64(numerator << 16U, denominator);
    return numerator > UINT32_MAX ? UINT32_MAX : (uint32_t)numerator;
}

static uint32_t ratio_permille(uint64_t numerator, uint64_t denominator)
{
    uint64_t scaled;
    if (denominator == 0U) return 0U;
    if (!multiply64(numerator, UINT64_C(1000), &scaled)) return UINT32_MAX;
    numerator = divide64(scaled, denominator);
    return numerator > UINT32_MAX ? UINT32_MAX : (uint32_t)numerator;
}

static int timing_delta(const mtfs_sentinel_timing_t *current,
    const mtfs_sentinel_timing_t *previous,
    mtfs_sentinel_operation_feature_t *operation)
{
    uint32_t i;
    if (!delta64(current->sample_count, previous->sample_count,
            &operation->timing_samples) ||
        !delta64(current->invalid_samples, previous->invalid_samples,
            &operation->timing_invalid) ||
        !delta64(current->total_latency_us, previous->total_latency_us,
            &operation->total_latency_us)) return 0;
    for (i = 0U; i < MTFS_SENTINEL_HISTOGRAM_BUCKETS; ++i) {
        if (!delta64(current->histogram[i], previous->histogram[i],
                &operation->latency_histogram[i])) return 0;
    }
    if (operation->timing_samples != 0U)
        operation->average_latency_us = divide64(operation->total_latency_us,
            operation->timing_samples);
    return 1;
}

static int add_product(uint64_t *sum, uint64_t value, uint64_t count)
{
    uint64_t product;
    if (!multiply64(value, count, &product) ||
        product > UINT64_MAX - *sum) return 0;
    *sum += product;
    return 1;
}

int mtfs_sentinel_operation_timing_is_consistent(
    const mtfs_sentinel_operation_feature_t *operation)
{
    uint64_t histogram_samples = 0U;
    uint64_t minimum_total_us = 0U;
    uint64_t maximum_total_us = 0U;
    uint32_t bucket;
    int maximum_unbounded = 0;
    if (operation == NULL) return 0;
    for (bucket = 0U; bucket < MTFS_SENTINEL_HISTOGRAM_BUCKETS; ++bucket) {
        uint64_t count = operation->latency_histogram[bucket];
        uint64_t minimum_us = bucket == 0U ? 0U :
            mtfs_sentinel_histogram_upper_us[bucket - 1U] + UINT64_C(1);
        if (count > UINT64_MAX - histogram_samples ||
            !add_product(&minimum_total_us, minimum_us, count)) return 0;
        histogram_samples += count;
        if (bucket == MTFS_SENTINEL_HISTOGRAM_BUCKETS - 1U) {
            if (count != 0U) maximum_unbounded = 1;
        } else if (!add_product(&maximum_total_us,
                mtfs_sentinel_histogram_upper_us[bucket], count)) {
            maximum_unbounded = 1;
        }
    }
    return histogram_samples == operation->timing_samples &&
        operation->total_latency_us >= minimum_total_us &&
        (maximum_unbounded ||
         operation->total_latency_us <= maximum_total_us) &&
        (operation->timing_samples == 0U ?
         operation->average_latency_us == 0U :
         operation->average_latency_us == divide64(
             operation->total_latency_us, operation->timing_samples));
}

static int diagnostics_delta(const mtfs_block_diagnostics_t *d,
    const mtfs_block_diagnostics_t *p, mtfs_sentinel_feature_v1_t *f)
{
    mtfs_sentinel_operation_feature_t *r = &f->operation[0];
    mtfs_sentinel_operation_feature_t *w = &f->operation[1];
    mtfs_sentinel_operation_feature_t *s = &f->operation[2];
    if (!delta32(d->read_calls, p->read_calls, &r->calls) ||
        !delta32(d->read_successes, p->read_successes, &r->successes) ||
        !delta32(d->read_failures, p->read_failures, &r->failures) ||
        !delta32(d->read_sectors_requested, p->read_sectors_requested,
            &r->sectors_requested) ||
        !delta32(d->read_sectors_completed, p->read_sectors_completed,
            &r->sectors_completed) ||
        !delta32(d->write_calls, p->write_calls, &w->calls) ||
        !delta32(d->write_successes, p->write_successes, &w->successes) ||
        !delta32(d->write_failures, p->write_failures, &w->failures) ||
        !delta32(d->write_sectors_requested, p->write_sectors_requested,
            &w->sectors_requested) ||
        !delta32(d->write_sectors_completed, p->write_sectors_completed,
            &w->sectors_completed) ||
        !delta32(d->sync_calls, p->sync_calls, &s->calls) ||
        !delta32(d->sync_successes, p->sync_successes, &s->successes) ||
        !delta32(d->sync_failures, p->sync_failures, &s->failures) ||
        !delta32(d->io_errors, p->io_errors, &f->io_errors) ||
        !delta32(d->not_ready_errors, p->not_ready_errors, &f->not_ready_errors) ||
        !delta32(d->no_media_errors, p->no_media_errors, &f->no_media_errors) ||
        !delta32(d->write_protected_errors, p->write_protected_errors,
            &f->write_protected_errors) ||
        !delta32(d->out_of_range_errors, p->out_of_range_errors,
            &f->out_of_range_errors) ||
        !delta32(d->timeout_errors, p->timeout_errors, &f->timeout_errors) ||
        !delta32(d->other_errors, p->other_errors, &f->other_errors)) return 0;
    r->average_sectors_per_request_q16 = ratio_q16(r->sectors_requested, r->calls);
    w->average_sectors_per_request_q16 = ratio_q16(w->sectors_requested, w->calls);
    r->sector_completion_permille = ratio_permille(
        r->sectors_completed, r->sectors_requested);
    w->sector_completion_permille = ratio_permille(
        w->sectors_completed, w->sectors_requested);
    return 1;
}

static int diagnostics_saturated(const mtfs_block_diagnostics_t *d)
{
    return d->read_calls == UINT32_MAX || d->write_calls == UINT32_MAX ||
        d->sync_calls == UINT32_MAX || d->read_sectors_requested == UINT32_MAX ||
        d->write_sectors_requested == UINT32_MAX || d->io_errors == UINT32_MAX ||
        d->not_ready_errors == UINT32_MAX || d->no_media_errors == UINT32_MAX ||
        d->other_errors == UINT32_MAX;
}

mtfs_error_t mtfs_sentinel_init(
    mtfs_sentinel_context_t *context, const mtfs_sentinel_config_t *config)
{
    if (context == NULL || config == NULL || config->observer == NULL ||
        config->clock == NULL || config->target_id == 0U ||
        config->transport_id == 0U) return MTFS_ERROR_INVALID_ARGUMENT;
    (void)memset(context, 0, sizeof(*context));
    context->config = *config;
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_reset(mtfs_sentinel_context_t *context)
{
    if (context == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    context->baseline_valid = 0U;
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_sample(mtfs_sentinel_context_t *context,
    const mtfs_sentinel_sample_metadata_t *metadata,
    mtfs_sentinel_feature_v1_t *feature)
{
    mtfs_block_diagnostics_t diagnostics;
    mtfs_sentinel_observer_snapshot_t observer;
    uint64_t now_us = 0U;
    int time_valid;
    int discontinuity = 0;
    if (context == NULL || metadata == NULL || feature == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (mtfs_block_diagnostics_get(
            mtfs_sentinel_observer_block_device(context->config.observer),
            &diagnostics) != MTFS_OK ||
        mtfs_sentinel_observer_get(context->config.observer, &observer) != MTFS_OK)
        return MTFS_ERROR_NOT_READY;
    time_valid = context->config.clock(context->config.clock_context,
        &now_us) == MTFS_OK;
    (void)memset(feature, 0, sizeof(*feature));
    feature->version = MTFS_SENTINEL_SCHEMA_VERSION;
    feature->struct_size = (uint16_t)sizeof(*feature);
    feature->target_id = context->config.target_id;
    feature->transport_id = context->config.transport_id;
    feature->media_generation = metadata->media_generation;
    feature->diagnostics_reset_epoch = diagnostics.reset_epoch;
    feature->observer_reset_epoch = observer.reset_epoch;
    feature->timestamp_us = now_us;
    feature->sample_count = 1U;
    feature->validity_mask = MTFS_SENTINEL_VALID_IDENTITY |
        MTFS_SENTINEL_VALID_MEDIA;
    if (!context->baseline_valid) {
        feature->flags |= MTFS_SENTINEL_FLAG_INSUFFICIENT_DATA;
        goto save_baseline;
    }
    if (diagnostics.reset_epoch != context->previous_diagnostics.reset_epoch ||
        observer.reset_epoch != context->previous_observer.reset_epoch ||
        metadata->media_generation != context->previous_media.media_generation ||
        metadata->media_reset_epoch != context->previous_media.media_reset_epoch ||
        context->config.target_id != context->previous_target_id ||
        context->config.transport_id != context->previous_transport_id ||
        diagnostics_saturated(&diagnostics) ||
        (observer.flags & MTFS_SENTINEL_OBSERVER_FLAG_SATURATED) != 0U)
        discontinuity = 1;
    if (!time_valid) feature->flags |= MTFS_SENTINEL_FLAG_TIMING_UNAVAILABLE;
    else if (now_us < context->previous_timestamp_us) discontinuity = 1;
    else {
        feature->observation_interval_us = now_us - context->previous_timestamp_us;
        feature->validity_mask |= MTFS_SENTINEL_VALID_INTERVAL;
    }
    if (discontinuity) {
        feature->flags |= MTFS_SENTINEL_FLAG_DISCONTINUITY |
            MTFS_SENTINEL_FLAG_INSUFFICIENT_DATA;
        if (diagnostics_saturated(&diagnostics) ||
            (observer.flags & MTFS_SENTINEL_OBSERVER_FLAG_SATURATED) != 0U)
            feature->flags |= MTFS_SENTINEL_FLAG_COUNTER_SATURATED;
        (void)media_event_delta(metadata, &context->previous_media, feature);
        goto save_baseline;
    }
    if (!diagnostics_delta(&diagnostics, &context->previous_diagnostics, feature)) {
        feature->flags |= MTFS_SENTINEL_FLAG_DISCONTINUITY;
        goto save_baseline;
    }
    feature->validity_mask |= MTFS_SENTINEL_VALID_IO_COUNTERS |
        MTFS_SENTINEL_VALID_ERRORS;
    if (timing_delta(&observer.read, &context->previous_observer.read,
            &feature->operation[0]) &&
        mtfs_sentinel_operation_timing_is_consistent(&feature->operation[0]))
        feature->validity_mask |= MTFS_SENTINEL_VALID_TIMING_READ;
    else feature->flags |= MTFS_SENTINEL_FLAG_DISCONTINUITY |
        MTFS_SENTINEL_FLAG_TIMING_UNAVAILABLE;
    if (timing_delta(&observer.write, &context->previous_observer.write,
            &feature->operation[1]) &&
        mtfs_sentinel_operation_timing_is_consistent(&feature->operation[1]))
        feature->validity_mask |= MTFS_SENTINEL_VALID_TIMING_WRITE;
    else feature->flags |= MTFS_SENTINEL_FLAG_DISCONTINUITY |
        MTFS_SENTINEL_FLAG_TIMING_UNAVAILABLE;
    if (timing_delta(&observer.sync, &context->previous_observer.sync,
            &feature->operation[2]) &&
        mtfs_sentinel_operation_timing_is_consistent(&feature->operation[2]))
        feature->validity_mask |= MTFS_SENTINEL_VALID_TIMING_SYNC;
    else feature->flags |= MTFS_SENTINEL_FLAG_DISCONTINUITY |
        MTFS_SENTINEL_FLAG_TIMING_UNAVAILABLE;
    if (feature->operation[0].timing_invalid != 0U ||
        feature->operation[1].timing_invalid != 0U ||
        feature->operation[2].timing_invalid != 0U)
        feature->flags |= MTFS_SENTINEL_FLAG_TIMING_UNAVAILABLE;
    if (!media_event_delta(metadata, &context->previous_media, feature))
        feature->flags |= MTFS_SENTINEL_FLAG_DISCONTINUITY;
    if ((feature->operation[0].calls + feature->operation[1].calls +
         feature->operation[2].calls) == 0U)
        feature->flags |= MTFS_SENTINEL_FLAG_NO_ACTIVITY;
    if (context->config.transport_sample != NULL &&
        context->config.transport_sample(context->config.transport_context,
            &feature->transport) == MTFS_OK)
        feature->validity_mask |= MTFS_SENTINEL_VALID_TRANSPORT;
save_baseline:
    context->previous_diagnostics = diagnostics;
    context->previous_observer = observer;
    context->previous_media = *metadata;
    context->previous_target_id = context->config.target_id;
    context->previous_transport_id = context->config.transport_id;
    if (time_valid) context->previous_timestamp_us = now_us;
    else feature->flags |= MTFS_SENTINEL_FLAG_TIMING_UNAVAILABLE;
    context->baseline_valid = time_valid ? 1U : 0U;
    ++context->sample_sequence;
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_window_init(mtfs_sentinel_window_t *window,
    mtfs_sentinel_feature_v1_t *storage, uint32_t capacity)
{
    if (window == NULL || storage == NULL || capacity == 0U)
        return MTFS_ERROR_INVALID_ARGUMENT;
    (void)memset(window, 0, sizeof(*window));
    window->storage = storage;
    window->capacity = capacity;
    return MTFS_OK;
}

void mtfs_sentinel_window_reset(mtfs_sentinel_window_t *window)
{
    if (window != NULL) { window->count = 0U; window->next = 0U; }
}

mtfs_error_t mtfs_sentinel_window_push(mtfs_sentinel_window_t *window,
    const mtfs_sentinel_feature_v1_t *feature)
{
    if (window == NULL || feature == NULL || window->storage == NULL ||
        feature->version != MTFS_SENTINEL_SCHEMA_VERSION ||
        feature->struct_size != sizeof(*feature)) return MTFS_ERROR_INVALID_ARGUMENT;
    if ((feature->flags & (MTFS_SENTINEL_FLAG_DISCONTINUITY |
            MTFS_SENTINEL_FLAG_COUNTER_SATURATED)) != 0U)
        mtfs_sentinel_window_reset(window);
    else if (window->count != 0U) {
        uint32_t newest = (window->next + window->capacity - 1U) % window->capacity;
        if (window->storage[newest].target_id != feature->target_id ||
            window->storage[newest].transport_id != feature->transport_id ||
            window->storage[newest].version != feature->version)
            mtfs_sentinel_window_reset(window);
    }
    window->storage[window->next] = *feature;
    window->next = (window->next + 1U) % window->capacity;
    if (window->count < window->capacity) ++window->count;
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_window_get(const mtfs_sentinel_window_t *window,
    mtfs_sentinel_feature_v1_t *feature)
{
    uint32_t n, op, bucket, index;
    const mtfs_sentinel_feature_v1_t *f;
    if (window == NULL || feature == NULL || window->storage == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (window->count == 0U) return MTFS_ERROR_NOT_READY;
    (void)memset(feature, 0, sizeof(*feature));
    index = (window->next + window->capacity - window->count) % window->capacity;
    *feature = window->storage[index];
    for (op = 0U; op < MTFS_SENTINEL_OPERATION_COUNT; ++op)
        (void)memset(&feature->operation[op], 0,
            sizeof(feature->operation[op]));
    feature->io_errors = feature->not_ready_errors = feature->no_media_errors = 0U;
    feature->write_protected_errors = feature->out_of_range_errors = 0U;
    feature->timeout_errors = feature->other_errors = 0U;
    feature->inserted_events = feature->removed_events = feature->media_error_events = 0U;
    feature->observation_interval_us = 0U;
    feature->sample_count = 0U;
    for (n = 0U; n < window->count; ++n) {
        f = &window->storage[(index + n) % window->capacity];
        feature->validity_mask &= f->validity_mask;
        feature->flags |= f->flags;
        feature->timestamp_us = f->timestamp_us;
        feature->media_generation = f->media_generation;
        feature->sample_count += f->sample_count;
        feature->observation_interval_us = add64(feature->observation_interval_us,
            f->observation_interval_us, &feature->flags);
        for (op = 0U; op < MTFS_SENTINEL_OPERATION_COUNT; ++op) {
            mtfs_sentinel_operation_feature_t *a = &feature->operation[op];
            const mtfs_sentinel_operation_feature_t *b = &f->operation[op];
            a->calls = add64(a->calls, b->calls, &feature->flags);
            a->successes = add64(a->successes, b->successes, &feature->flags);
            a->failures = add64(a->failures, b->failures, &feature->flags);
            a->sectors_requested = add64(a->sectors_requested,
                b->sectors_requested, &feature->flags);
            a->sectors_completed = add64(a->sectors_completed,
                b->sectors_completed, &feature->flags);
            a->timing_samples = add64(a->timing_samples,
                b->timing_samples, &feature->flags);
            a->timing_invalid = add64(a->timing_invalid,
                b->timing_invalid, &feature->flags);
            a->total_latency_us = add64(a->total_latency_us,
                b->total_latency_us, &feature->flags);
            for (bucket = 0U; bucket < MTFS_SENTINEL_HISTOGRAM_BUCKETS; ++bucket)
                a->latency_histogram[bucket] = add64(
                    a->latency_histogram[bucket], b->latency_histogram[bucket],
                    &feature->flags);
        }
        feature->io_errors = add64(feature->io_errors, f->io_errors, &feature->flags);
        feature->not_ready_errors = add64(feature->not_ready_errors,
            f->not_ready_errors, &feature->flags);
        feature->no_media_errors = add64(feature->no_media_errors,
            f->no_media_errors, &feature->flags);
        feature->write_protected_errors = add64(feature->write_protected_errors,
            f->write_protected_errors, &feature->flags);
        feature->out_of_range_errors = add64(feature->out_of_range_errors,
            f->out_of_range_errors, &feature->flags);
        feature->timeout_errors = add64(feature->timeout_errors,
            f->timeout_errors, &feature->flags);
        feature->other_errors = add64(feature->other_errors,
            f->other_errors, &feature->flags);
        feature->inserted_events += f->inserted_events;
        feature->removed_events += f->removed_events;
        feature->media_error_events += f->media_error_events;
    }
    for (op = 0U; op < MTFS_SENTINEL_OPERATION_COUNT; ++op) {
        mtfs_sentinel_operation_feature_t *a = &feature->operation[op];
        a->average_sectors_per_request_q16 = ratio_q16(a->sectors_requested, a->calls);
        a->sector_completion_permille = ratio_permille(
            a->sectors_completed, a->sectors_requested);
        if (a->timing_samples != 0U)
            a->average_latency_us = divide64(
                a->total_latency_us, a->timing_samples);
    }
    return MTFS_OK;
}

#endif /* MTFS_ENABLE_STORAGE_SENTINEL */
