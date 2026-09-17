/** @file mtfs_media_service.h
 * @brief Optional microT-Kernel worker for removable-media debounce. / removable mediaのdebounce処理を行う、任意使用のmicroT-Kernel worker。
 * @details The caller owns the concrete context and must stop notification acceptance, disable/unregister the source IRQ, then deinitialize. ISR notify only records state and sets an event flag; callbacks run in task context.
 * / contextは呼び出し側が所有する。終了時はnotificationの受付停止、発生元IRQのdisable／unregister、deinitの順で処理する必要がある。ISRからのnotifyではstateの記録とevent flagの設定のみを行い、callbackはtask contextで実行する。
 * @ingroup mtfs_kernel */
#ifndef MTFS_MEDIA_SERVICE_H
#define MTFS_MEDIA_SERVICE_H

/** @addtogroup mtfs_kernel
 * @{ */

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
 * RAM and kernel objects exist only when an application explicitly allocates this context and calls init.  No singleton or implicit worker is created.
 */
/*
 * applicationがこのcontextを明示的に確保してinitを呼び出した場合にのみ、RAMとkernel objectが使用される。singletonや暗黙的なworkerは生成されない。
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
/* 終了処理の最初に呼び出し、以降のISR notificationを無視できる状態にする。 */
mtfs_error_t mtfs_media_service_stop_notifications(
    mtfs_media_service_context_t *context);

/*
 * ISR-safe when called from a microT-Kernel high-level interrupt handler. It records the level and calls tk_set_flg(); no callback runs here.
 */
/*
 * microT-Kernelのhigh-level interrupt handlerから安全に呼び出せる。raw levelを記録してtk_set_flg()を呼び出すだけで、この処理内ではcallbackを実行しない。
 */
mtfs_error_t mtfs_media_service_notify_isr(
    mtfs_media_service_context_t *context, int raw_level);

/* Call only after the source IRQ has been disabled and unregistered. */
/* 発生元IRQをdisableし、unregisterした後にのみ呼び出すこと。 */
mtfs_error_t mtfs_media_service_deinit(
    mtfs_media_service_context_t *context);

#ifdef __cplusplus
}
#endif

/** @} */
#endif /* MTFS_MEDIA_SERVICE_H */
