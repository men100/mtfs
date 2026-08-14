/* Optional microT-Kernel worker for the removable-media state machine. */
#ifndef MTFS_MEDIA_SERVICE_H
#define MTFS_MEDIA_SERVICE_H

#include <stdint.h>

#include <tk/tkernel.h>

#include "../../core/mtfs_media.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MTFS_MEDIA_SERVICE_STACK_SIZE
#define MTFS_MEDIA_SERVICE_STACK_SIZE (2048U)
#endif

#ifndef MTFS_MEDIA_SERVICE_STOP_TIMEOUT_MS
#define MTFS_MEDIA_SERVICE_STOP_TIMEOUT_MS (1000U)
#endif

typedef uint32_t (*mtfs_media_service_now_fn)(void *opaque);

typedef struct mtfs_media_service_config
{
    mtfs_media_context_t *media;
    mtfs_media_service_now_fn now_ms;
    void *clock_context;
    PRI task_priority;
} mtfs_media_service_config_t;

/*
 * RAM and kernel objects exist only when an application explicitly allocates
 * this context and calls init.  No singleton or implicit worker is created.
 */
typedef struct mtfs_media_service_context
{
    mtfs_media_service_config_t config;
    ID task_id;
    ID event_flag_id;
    volatile ER last_kernel_error;
    volatile uint8_t accepting_notifications;
    uint8_t worker_stack[MTFS_MEDIA_SERVICE_STACK_SIZE]
        __attribute__((aligned(8)));
} mtfs_media_service_context_t;

mtfs_error_t mtfs_media_service_init(
    mtfs_media_service_context_t *context,
    const mtfs_media_service_config_t *config);

/* First shutdown step: makes subsequent ISR notifications harmless. */
mtfs_error_t mtfs_media_service_stop_notifications(
    mtfs_media_service_context_t *context);

/*
 * ISR-safe when called from a microT-Kernel high-level interrupt handler.
 * It records the level and calls tk_set_flg(); no callback runs here.
 */
mtfs_error_t mtfs_media_service_notify_isr(
    mtfs_media_service_context_t *context, int raw_level);

/* Call only after the source IRQ has been disabled and unregistered. */
mtfs_error_t mtfs_media_service_deinit(
    mtfs_media_service_context_t *context);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_MEDIA_SERVICE_H */
