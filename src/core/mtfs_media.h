/* Platform-independent removable-media lifecycle state machine. */
#ifndef MTFS_MEDIA_H
#define MTFS_MEDIA_H

#include <stdint.h>

#include "../mtfs_config.h"
#include "../mtfs_error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_MEDIA_WAIT_FOREVER (UINT32_MAX)
#define MTFS_MEDIA_DIAGNOSTICS_API_VERSION (UINT16_C(1))
#define MTFS_MEDIA_DIAGNOSTICS_VALID_STATE (UINT32_C(1) << 0)
#define MTFS_MEDIA_DIAGNOSTICS_VALID_STABLE_PRESENT (UINT32_C(1) << 1)
#define MTFS_MEDIA_DIAGNOSTICS_VALID_NOTIFICATION_SEQUENCE (UINT32_C(1) << 2)
#define MTFS_MEDIA_DIAGNOSTICS_VALID_MEDIA_GENERATION (UINT32_C(1) << 3)

typedef enum mtfs_media_state
{
    MTFS_MEDIA_STATE_ABSENT = 0,
    MTFS_MEDIA_STATE_DEBOUNCING_INSERT,
    MTFS_MEDIA_STATE_PRESENT,
    MTFS_MEDIA_STATE_DEBOUNCING_REMOVE,
    MTFS_MEDIA_STATE_ERROR
} mtfs_media_state_t;

typedef enum mtfs_media_event
{
    MTFS_MEDIA_EVENT_INSERTED = 0,
    MTFS_MEDIA_EVENT_REMOVED,
    MTFS_MEDIA_EVENT_ERROR
} mtfs_media_event_t;

typedef enum mtfs_media_active_level
{
    MTFS_MEDIA_ACTIVE_LOW = 0,
    MTFS_MEDIA_ACTIVE_HIGH = 1
} mtfs_media_active_level_t;

/* Return 0/1 for the raw GPIO level and a negative value on read failure. */
typedef int (*mtfs_media_read_signal_fn)(void *opaque);
typedef void (*mtfs_media_event_fn)(
    void *opaque, mtfs_media_event_t event, mtfs_media_state_t state);

typedef struct mtfs_media_config
{
    mtfs_media_read_signal_fn read_signal;
    void *signal_context;
    mtfs_media_event_fn event_callback;
    void *event_context;
    uint32_t debounce_ms;
    mtfs_media_active_level_t active_level;
} mtfs_media_config_t;

typedef struct mtfs_media_diagnostics
{
    uint16_t api_version;
    uint16_t struct_size;
    uint32_t validity_mask;
    uint32_t reset_epoch;
    uint32_t media_generation;
    uint32_t notification_sequence;
    uint32_t state;
    uint8_t stable_present;
    uint8_t reserved[3];
    uint32_t irq_notifications;
    uint32_t manual_notifications;
    uint32_t poll_checks;
    uint32_t debounce_starts;
    uint32_t debounce_rechecks;
    uint32_t inserted_events;
    uint32_t removed_events;
    uint32_t error_events;
} mtfs_media_diagnostics_t;

/* Concrete by design: applications statically allocate this object. */
typedef struct mtfs_media_context
{
    mtfs_media_config_t config;
    volatile uint32_t notification_sequence;
    volatile uint8_t notified_raw_level;
    volatile uint8_t accepting_notifications;
    uint32_t observed_sequence;
    uint32_t debounce_deadline_ms;
    mtfs_media_state_t state;
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_media_diagnostics_t diagnostics;
    uint32_t media_generation;
#endif
    uint8_t stable_present;
    uint8_t candidate_present;
    uint8_t last_polled_raw_level;
    uint8_t debouncing;
    uint8_t initialized;
} mtfs_media_context_t;

mtfs_error_t mtfs_media_init(
    mtfs_media_context_t *context, const mtfs_media_config_t *config);

/* Stop notification acceptance before disabling/deleting the source IRQ. */
mtfs_error_t mtfs_media_stop_notifications(mtfs_media_context_t *context);
mtfs_error_t mtfs_media_deinit(mtfs_media_context_t *context);

/*
 * ISR-safe: only records the raw level and a sequence number.  It never reads
 * GPIO, waits, allocates, locks, or invokes the application callback.
 */
mtfs_error_t mtfs_media_notify_isr(
    mtfs_media_context_t *context, int raw_level);

/* Task-context manual edge notification; rereads the configured signal. */
mtfs_error_t mtfs_media_notify(mtfs_media_context_t *context);

/*
 * Task-context processing. next_wait_ms is the remaining one-shot debounce
 * delay, or MTFS_MEDIA_WAIT_FOREVER when no work is pending.
 */
mtfs_error_t mtfs_media_process(
    mtfs_media_context_t *context, uint32_t now_ms,
    uint32_t *next_wait_ms);

/* Explicit fallback only; applications choose if and when to call it. */
mtfs_error_t mtfs_media_poll(
    mtfs_media_context_t *context, uint32_t now_ms,
    uint32_t *next_wait_ms);

mtfs_media_state_t mtfs_media_state(const mtfs_media_context_t *context);
int mtfs_media_is_present(const mtfs_media_context_t *context);
/* Task-context, cached-state-only diagnostics. */
mtfs_error_t mtfs_media_diagnostics_get(
    const mtfs_media_context_t *context,
    mtfs_media_diagnostics_t *snapshot);
mtfs_error_t mtfs_media_diagnostics_reset(mtfs_media_context_t *context);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_MEDIA_H */
