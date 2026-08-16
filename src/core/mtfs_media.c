#include "mtfs_media.h"

#include <stddef.h>
#include <string.h>

#if MTFS_ENABLE_DIAGNOSTICS
static void mtfs_media_diagnostic_increment(uint32_t *counter)
{
    if (*counter != UINT32_MAX) {
        ++*counter;
    }
}
#define MTFS_MEDIA_DIAG_INCREMENT(context_, field_) \
    mtfs_media_diagnostic_increment(&(context_)->diagnostics.field_)
#else
#define MTFS_MEDIA_DIAG_INCREMENT(context_, field_) ((void)(context_))
#endif

static int mtfs_media_deadline_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static uint8_t mtfs_media_raw_to_present(
    const mtfs_media_context_t *context, int raw_level)
{
    uint8_t high = (raw_level != 0) ? 1U : 0U;
    return (context->config.active_level == MTFS_MEDIA_ACTIVE_HIGH)
        ? high : (uint8_t)!high;
}

static mtfs_error_t mtfs_media_read(
    mtfs_media_context_t *context, uint8_t *raw_level,
    uint8_t *present)
{
    int value = context->config.read_signal(context->config.signal_context);
    if (value < 0) {
        return MTFS_ERROR_IO;
    }
    *raw_level = (value != 0) ? 1U : 0U;
    *present = mtfs_media_raw_to_present(context, value);
    return MTFS_OK;
}

static mtfs_error_t mtfs_media_enter_error(mtfs_media_context_t *context)
{
    int notify = context->state != MTFS_MEDIA_STATE_ERROR;
    context->state = MTFS_MEDIA_STATE_ERROR;
    context->debouncing = 0U;
    if (notify) {
        MTFS_MEDIA_DIAG_INCREMENT(context, error_events);
        if (context->config.event_callback != NULL) {
            context->config.event_callback(context->config.event_context,
                MTFS_MEDIA_EVENT_ERROR, context->state);
        }
    }
    return MTFS_ERROR_IO;
}

static void mtfs_media_start_debounce(
    mtfs_media_context_t *context, uint8_t present, uint32_t now_ms)
{
    context->candidate_present = present;
    context->state = present ? MTFS_MEDIA_STATE_DEBOUNCING_INSERT
                             : MTFS_MEDIA_STATE_DEBOUNCING_REMOVE;
    context->debounce_deadline_ms = now_ms + context->config.debounce_ms;
    context->debouncing = 1U;
    MTFS_MEDIA_DIAG_INCREMENT(context, debounce_starts);
}

static void mtfs_media_settle(mtfs_media_context_t *context, uint8_t present)
{
    int changed = present != context->stable_present;
    context->stable_present = present;
    context->state = present ? MTFS_MEDIA_STATE_PRESENT
                             : MTFS_MEDIA_STATE_ABSENT;
    context->debouncing = 0U;
    if (!changed) {
        return;
    }
    if (present) {
        MTFS_MEDIA_DIAG_INCREMENT(context, inserted_events);
#if MTFS_ENABLE_DIAGNOSTICS
        ++context->media_generation;
#endif
        if (context->config.event_callback != NULL) {
            context->config.event_callback(context->config.event_context,
                MTFS_MEDIA_EVENT_INSERTED, context->state);
        }
    } else {
        MTFS_MEDIA_DIAG_INCREMENT(context, removed_events);
#if MTFS_ENABLE_DIAGNOSTICS
        ++context->media_generation;
#endif
        if (context->config.event_callback != NULL) {
            context->config.event_callback(context->config.event_context,
                MTFS_MEDIA_EVENT_REMOVED, context->state);
        }
    }
}

mtfs_error_t mtfs_media_init(
    mtfs_media_context_t *context, const mtfs_media_config_t *config)
{
    uint8_t raw_level;
    uint8_t present;

    if ((context == NULL) || (config == NULL) ||
        (config->read_signal == NULL) ||
        (config->debounce_ms > INT32_MAX) ||
        ((config->active_level != MTFS_MEDIA_ACTIVE_LOW) &&
         (config->active_level != MTFS_MEDIA_ACTIVE_HIGH))) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    (void)memset(context, 0, sizeof(*context));
    context->config = *config;
    context->state = MTFS_MEDIA_STATE_ERROR;
#if MTFS_ENABLE_DIAGNOSTICS
    context->diagnostics.api_version = MTFS_MEDIA_DIAGNOSTICS_API_VERSION;
    context->diagnostics.struct_size = (uint16_t)sizeof(context->diagnostics);
    context->diagnostics.validity_mask =
        MTFS_MEDIA_DIAGNOSTICS_VALID_STATE |
        MTFS_MEDIA_DIAGNOSTICS_VALID_STABLE_PRESENT |
        MTFS_MEDIA_DIAGNOSTICS_VALID_NOTIFICATION_SEQUENCE |
        MTFS_MEDIA_DIAGNOSTICS_VALID_MEDIA_GENERATION;
#endif
    if (mtfs_media_read(context, &raw_level, &present) != MTFS_OK) {
        context->initialized = 1U;
        context->accepting_notifications = 1U;
        (void)mtfs_media_enter_error(context);
        return MTFS_ERROR_IO;
    }
    context->stable_present = present;
#if MTFS_ENABLE_DIAGNOSTICS
    context->media_generation = present ? 1U : 0U;
#endif
    context->candidate_present = present;
    context->last_polled_raw_level = raw_level;
    context->notified_raw_level = raw_level;
    context->state = present ? MTFS_MEDIA_STATE_PRESENT
                             : MTFS_MEDIA_STATE_ABSENT;
    context->initialized = 1U;
    context->accepting_notifications = 1U;
    return MTFS_OK;
}

mtfs_error_t mtfs_media_stop_notifications(mtfs_media_context_t *context)
{
    if ((context == NULL) || !context->initialized) {
        return MTFS_ERROR_NOT_READY;
    }
    context->accepting_notifications = 0U;
    return MTFS_OK;
}

mtfs_error_t mtfs_media_deinit(mtfs_media_context_t *context)
{
    if ((context == NULL) || !context->initialized) {
        return MTFS_ERROR_NOT_READY;
    }
    context->accepting_notifications = 0U;
    context->initialized = 0U;
    context->debouncing = 0U;
    return MTFS_OK;
}

mtfs_error_t mtfs_media_notify_isr(
    mtfs_media_context_t *context, int raw_level)
{
    if ((context == NULL) || !context->initialized ||
        !context->accepting_notifications) {
        return MTFS_ERROR_NOT_READY;
    }
    context->notified_raw_level = (raw_level != 0) ? 1U : 0U;
    ++context->notification_sequence;
    MTFS_MEDIA_DIAG_INCREMENT(context, irq_notifications);
    return MTFS_OK;
}

mtfs_error_t mtfs_media_notify(mtfs_media_context_t *context)
{
    uint8_t raw_level;
    uint8_t present;
    mtfs_error_t result;

    if ((context == NULL) || !context->initialized ||
        !context->accepting_notifications) {
        return MTFS_ERROR_NOT_READY;
    }
    result = mtfs_media_read(context, &raw_level, &present);
    (void)present;
    if (result != MTFS_OK) {
        return mtfs_media_enter_error(context);
    }
    context->notified_raw_level = raw_level;
    ++context->notification_sequence;
    MTFS_MEDIA_DIAG_INCREMENT(context, manual_notifications);
    return MTFS_OK;
}

mtfs_error_t mtfs_media_process(
    mtfs_media_context_t *context, uint32_t now_ms,
    uint32_t *next_wait_ms)
{
    uint32_t sequence;
    uint8_t raw_level;
    uint8_t present;

    if ((context == NULL) || (next_wait_ms == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    *next_wait_ms = MTFS_MEDIA_WAIT_FOREVER;
    if (!context->initialized) {
        return MTFS_ERROR_NOT_READY;
    }

    sequence = context->notification_sequence;
    if (sequence != context->observed_sequence) {
        if (mtfs_media_read(context, &raw_level, &present) != MTFS_OK) {
            return mtfs_media_enter_error(context);
        }
        context->last_polled_raw_level = raw_level;
        /* Keep the pre-read snapshot so an IRQ racing the read stays pending. */
        context->observed_sequence = sequence;
        mtfs_media_start_debounce(context, present, now_ms);
    }

    if (!context->debouncing) {
        return MTFS_OK;
    }
    if (!mtfs_media_deadline_reached(now_ms,
            context->debounce_deadline_ms)) {
        *next_wait_ms = context->debounce_deadline_ms - now_ms;
        return MTFS_OK;
    }

    sequence = context->notification_sequence;
    if (mtfs_media_read(context, &raw_level, &present) != MTFS_OK) {
        return mtfs_media_enter_error(context);
    }
    context->last_polled_raw_level = raw_level;
    MTFS_MEDIA_DIAG_INCREMENT(context, debounce_rechecks);
    if ((sequence != context->notification_sequence) ||
        (present != context->candidate_present)) {
        /* A sequence change during the read is deliberately left pending. */
        context->observed_sequence = sequence;
        mtfs_media_start_debounce(context, present, now_ms);
        *next_wait_ms = context->config.debounce_ms;
        return MTFS_OK;
    }
    mtfs_media_settle(context, present);
    return MTFS_OK;
}

mtfs_error_t mtfs_media_poll(
    mtfs_media_context_t *context, uint32_t now_ms,
    uint32_t *next_wait_ms)
{
    uint8_t raw_level;
    uint8_t present;

    if ((context == NULL) || (next_wait_ms == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    if (!context->initialized || !context->accepting_notifications) {
        return MTFS_ERROR_NOT_READY;
    }
    MTFS_MEDIA_DIAG_INCREMENT(context, poll_checks);
    if (mtfs_media_read(context, &raw_level, &present) != MTFS_OK) {
        return mtfs_media_enter_error(context);
    }
    (void)present;
    if (raw_level != context->last_polled_raw_level) {
        context->last_polled_raw_level = raw_level;
        context->notified_raw_level = raw_level;
        ++context->notification_sequence;
    }
    return mtfs_media_process(context, now_ms, next_wait_ms);
}

mtfs_media_state_t mtfs_media_state(const mtfs_media_context_t *context)
{
    return ((context == NULL) || !context->initialized)
        ? MTFS_MEDIA_STATE_ERROR : context->state;
}

int mtfs_media_is_present(const mtfs_media_context_t *context)
{
    return (context != NULL) && context->initialized &&
        (context->state == MTFS_MEDIA_STATE_PRESENT);
}

mtfs_error_t mtfs_media_diagnostics_get(
    const mtfs_media_context_t *context,
    mtfs_media_diagnostics_t *snapshot)
{
    if ((context == NULL) || (snapshot == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
#if MTFS_ENABLE_DIAGNOSTICS
    if (context->diagnostics.api_version !=
        MTFS_MEDIA_DIAGNOSTICS_API_VERSION) {
        return MTFS_ERROR_NOT_READY;
    }
    *snapshot = context->diagnostics;
    snapshot->media_generation = context->media_generation;
    snapshot->notification_sequence = context->notification_sequence;
    snapshot->state = (uint32_t)context->state;
    snapshot->stable_present = context->stable_present;
    return MTFS_OK;
#else
    return MTFS_ERROR_NOT_SUPPORTED;
#endif
}

mtfs_error_t mtfs_media_diagnostics_reset(mtfs_media_context_t *context)
{
    if (context == NULL) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
#if MTFS_ENABLE_DIAGNOSTICS
    {
        uint32_t epoch;
        if (context->diagnostics.api_version !=
            MTFS_MEDIA_DIAGNOSTICS_API_VERSION) {
            return MTFS_ERROR_NOT_READY;
        }
        epoch = context->diagnostics.reset_epoch + 1U;
        (void)memset(&context->diagnostics, 0, sizeof(context->diagnostics));
        context->diagnostics.api_version = MTFS_MEDIA_DIAGNOSTICS_API_VERSION;
        context->diagnostics.struct_size =
            (uint16_t)sizeof(context->diagnostics);
        context->diagnostics.validity_mask =
            MTFS_MEDIA_DIAGNOSTICS_VALID_STATE |
            MTFS_MEDIA_DIAGNOSTICS_VALID_STABLE_PRESENT |
            MTFS_MEDIA_DIAGNOSTICS_VALID_NOTIFICATION_SEQUENCE |
            MTFS_MEDIA_DIAGNOSTICS_VALID_MEDIA_GENERATION;
        context->diagnostics.reset_epoch = epoch;
        return MTFS_OK;
    }
#else
    return MTFS_ERROR_NOT_SUPPORTED;
#endif
}
