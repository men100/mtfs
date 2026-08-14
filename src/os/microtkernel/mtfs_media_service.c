#include "mtfs_media_service.h"

#include <string.h>

#define MTFS_MEDIA_SERVICE_EVENT_CHANGE  (UINT32_C(1) << 0)
#define MTFS_MEDIA_SERVICE_EVENT_STOP    (UINT32_C(1) << 1)
#define MTFS_MEDIA_SERVICE_EVENT_STOPPED (UINT32_C(1) << 2)

#define MTFS_MEDIA_SERVICE_DEFAULT_PRIORITY (10)

static void mtfs_media_service_worker(INT start_code, void *opaque)
{
    mtfs_media_service_context_t *context = opaque;
    UINT events;
    uint32_t wait_ms = MTFS_MEDIA_WAIT_FOREVER;
    (void)start_code;

    for (;;) {
        TMO timeout = (wait_ms == MTFS_MEDIA_WAIT_FOREVER)
            ? TMO_FEVR : (TMO)wait_ms;
        ER result = tk_wai_flg(context->event_flag_id,
            MTFS_MEDIA_SERVICE_EVENT_CHANGE | MTFS_MEDIA_SERVICE_EVENT_STOP,
            TWF_ORW | TWF_BITCLR, &events, timeout);

        context->last_kernel_error = result;
        if ((result >= E_OK) &&
            ((events & MTFS_MEDIA_SERVICE_EVENT_STOP) != 0U)) {
            break;
        }
        if ((result < E_OK) && (MERCD(result) != MERCD(E_TMOUT))) {
            wait_ms = MTFS_MEDIA_WAIT_FOREVER;
            continue;
        }
        (void)mtfs_media_process(context->config.media,
            context->config.now_ms(context->config.clock_context), &wait_ms);
    }

    (void)tk_set_flg(context->event_flag_id,
        MTFS_MEDIA_SERVICE_EVENT_STOPPED);
    tk_ext_tsk();
}

/*
 * The coordinator normally has a higher priority than the worker.  Therefore
 * tk_set_flg(STOPPED) can dispatch the coordinator before the worker reaches
 * tk_ext_tsk(), and tk_del_tsk() then correctly returns E_OBJ because the
 * worker is still READY.  The STOPPED flag is a final-use boundary: after it
 * is posted the worker only calls tk_ext_tsk() and no longer accesses the
 * service context or event flag.  If deletion observes that small window,
 * terminate the stopped worker first and then delete its dormant task object.
 */
static ER mtfs_media_service_delete_stopped_worker(
    mtfs_media_service_context_t *context)
{
    ER result = tk_del_tsk(context->task_id);

    if ((result < E_OK) && (MERCD(result) == MERCD(E_OBJ))) {
        result = tk_ter_tsk(context->task_id);
        if ((result < E_OK) && (MERCD(result) != MERCD(E_OBJ))) {
            return result;
        }
        result = tk_del_tsk(context->task_id);
    }
    return result;
}

mtfs_error_t mtfs_media_service_init(
    mtfs_media_service_context_t *context,
    const mtfs_media_service_config_t *config)
{
    T_CFLG flag_config = {0};
    T_CTSK task_config = {0};

    if ((context == NULL) || (config == NULL) ||
        (config->media == NULL) || !config->media->initialized ||
        (config->now_ms == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    (void)memset(context, 0, sizeof(*context));
    context->config = *config;
    if (context->config.task_priority <= 0) {
        context->config.task_priority = MTFS_MEDIA_SERVICE_DEFAULT_PRIORITY;
    }

    flag_config.flgatr = TA_TFIFO | TA_WMUL;
    context->event_flag_id = tk_cre_flg(&flag_config);
    if (context->event_flag_id <= 0) {
        context->last_kernel_error = context->event_flag_id;
        context->event_flag_id = 0;
        return MTFS_ERROR_NOT_READY;
    }

    task_config.exinf = context;
    task_config.tskatr = TA_HLNG | TA_RNG3 | TA_USERBUF;
    task_config.task = mtfs_media_service_worker;
    task_config.itskpri = context->config.task_priority;
    task_config.stksz = MTFS_MEDIA_SERVICE_STACK_SIZE;
    task_config.bufptr = context->worker_stack;
    context->task_id = tk_cre_tsk(&task_config);
    if (context->task_id <= 0) {
        context->last_kernel_error = context->task_id;
        context->task_id = 0;
        (void)tk_del_flg(context->event_flag_id);
        context->event_flag_id = 0;
        return MTFS_ERROR_NOT_READY;
    }
    context->last_kernel_error = tk_sta_tsk(context->task_id, 0);
    if (context->last_kernel_error < E_OK) {
        (void)tk_del_tsk(context->task_id);
        (void)tk_del_flg(context->event_flag_id);
        context->task_id = 0;
        context->event_flag_id = 0;
        return MTFS_ERROR_NOT_READY;
    }
    context->accepting_notifications = 1U;
    return MTFS_OK;
}

mtfs_error_t mtfs_media_service_stop_notifications(
    mtfs_media_service_context_t *context)
{
    if ((context == NULL) || (context->task_id <= 0) ||
        (context->event_flag_id <= 0)) {
        return MTFS_ERROR_NOT_READY;
    }
    context->accepting_notifications = 0U;
    return mtfs_media_stop_notifications(context->config.media);
}

mtfs_error_t mtfs_media_service_notify_isr(
    mtfs_media_service_context_t *context, int raw_level)
{
    mtfs_error_t result;
    ER kernel_result;

    if ((context == NULL) || !context->accepting_notifications ||
        (context->event_flag_id <= 0)) {
        return MTFS_ERROR_NOT_READY;
    }
    result = mtfs_media_notify_isr(context->config.media, raw_level);
    if (result != MTFS_OK) {
        return result;
    }
    kernel_result = tk_set_flg(context->event_flag_id,
        MTFS_MEDIA_SERVICE_EVENT_CHANGE);
    context->last_kernel_error = kernel_result;
    return (kernel_result < E_OK) ? MTFS_ERROR_IO : MTFS_OK;
}

mtfs_error_t mtfs_media_service_deinit(
    mtfs_media_service_context_t *context)
{
    UINT events = 0U;
    ER kernel_result;

    if ((context == NULL) || (context->task_id <= 0) ||
        (context->event_flag_id <= 0)) {
        return MTFS_ERROR_NOT_READY;
    }
    context->accepting_notifications = 0U;
    context->last_kernel_error = tk_set_flg(context->event_flag_id,
        MTFS_MEDIA_SERVICE_EVENT_STOP);
    if (context->last_kernel_error < E_OK) {
        return MTFS_ERROR_IO;
    }
    context->last_kernel_error = tk_wai_flg(context->event_flag_id,
        MTFS_MEDIA_SERVICE_EVENT_STOPPED, TWF_ANDW | TWF_BITCLR, &events,
        MTFS_MEDIA_SERVICE_STOP_TIMEOUT_MS);
    if (context->last_kernel_error < E_OK) {
        return MTFS_ERROR_NOT_READY;
    }
    kernel_result = mtfs_media_service_delete_stopped_worker(context);
    context->last_kernel_error = kernel_result;
    if (kernel_result < E_OK) {
        /* Keep both IDs valid so the caller cannot reuse a live context. */
        return MTFS_ERROR_IO;
    }
    context->task_id = 0;
    kernel_result = tk_del_flg(context->event_flag_id);
    context->last_kernel_error = kernel_result;
    if (kernel_result < E_OK) {
        return MTFS_ERROR_IO;
    }
    context->event_flag_id = 0;
    return MTFS_OK;
}
