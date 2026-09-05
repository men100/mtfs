#include "mtfs_stm32n6_aton_osal.h"

#include <limits.h>
#include <string.h>

#include <tk/tkernel.h>

#include "ll_aton_platform.h"

#define MTFS_STM32N6_ATON_EVENT (UINT32_C(1))
#define MTFS_STM32N6_ATON_IRQ_PRIORITY (6U)

static volatile ID event_flag_id;
static volatile uint8_t event_ready;
static volatile uint8_t inference_active;
static uint32_t inference_notification_start;
static uint32_t inference_consumed_start;
static volatile mtfs_stm32n6_aton_osal_diagnostics_t osal_diagnostics;

static void diagnostics_clear(void)
{
    osal_diagnostics.irq_notifications = 0U;
    osal_diagnostics.consumed_notifications = 0U;
    osal_diagnostics.spurious_notifications = 0U;
    osal_diagnostics.late_notifications = 0U;
    osal_diagnostics.kernel_signal_errors = 0U;
    osal_diagnostics.last_kernel_error = 0;
}

static int kernel_is_timeout(ER status)
{
    return MERCD(status) == MERCD(E_TMOUT);
}

static ER consume_event(TMO timeout)
{
    UINT events = 0U;
    return tk_wai_flg(event_flag_id, MTFS_STM32N6_ATON_EVENT,
        TWF_ORW | TWF_BITCLR, &events, timeout);
}

void mtfs_stm32n6_aton_osal_init(void)
{
    T_CFLG config = {0};
    ID created;
    if (event_flag_id > 0 || event_ready != 0U) {
        osal_diagnostics.last_kernel_error = E_OBJ;
        return;
    }
    config.flgatr = TA_TFIFO;
    created = tk_cre_flg(&config);
    if (created <= 0) {
        osal_diagnostics.last_kernel_error = created;
        return;
    }
    event_flag_id = created;
    inference_active = 0U;
    event_ready = 1U;
    NVIC_SetPriority(NPU0_IRQn, MTFS_STM32N6_ATON_IRQ_PRIORITY);
}

void mtfs_stm32n6_aton_osal_deinit(void)
{
    ID deleting = event_flag_id;
    inference_active = 0U;
    event_ready = 0U;
    event_flag_id = 0;
    if (deleting > 0) {
        ER status = tk_del_flg(deleting);
        if (status < E_OK) osal_diagnostics.last_kernel_error = status;
    }
}

void mtfs_stm32n6_aton_osal_wfe(void)
{
    if (event_ready != 0U && event_flag_id > 0)
        (void)consume_event(TMO_FEVR);
}

void mtfs_stm32n6_aton_osal_signal_event(void)
{
    ER status;
    ++osal_diagnostics.irq_notifications;
    if (event_ready == 0U || event_flag_id <= 0 || inference_active == 0U) {
        ++osal_diagnostics.late_notifications;
        return;
    }
    status = tk_set_flg(event_flag_id, MTFS_STM32N6_ATON_EVENT);
    if (status < E_OK) {
        ++osal_diagnostics.kernel_signal_errors;
        osal_diagnostics.last_kernel_error = status;
    }
}

int mtfs_stm32n6_aton_osal_ready(void)
{
    return event_ready != 0U && event_flag_id > 0;
}

mtfs_error_t mtfs_stm32n6_aton_osal_inference_begin(void)
{
    ER status;
    if (!mtfs_stm32n6_aton_osal_ready() || inference_active != 0U)
        return MTFS_ERROR_INVALID_STATE;
    status = consume_event(TMO_POL);
    if (status >= E_OK) ++osal_diagnostics.late_notifications;
    else if (!kernel_is_timeout(status)) {
        osal_diagnostics.last_kernel_error = status;
        return MTFS_ERROR_IO;
    }
    inference_notification_start = osal_diagnostics.irq_notifications;
    inference_consumed_start = osal_diagnostics.consumed_notifications;
    inference_active = 1U;
    return MTFS_OK;
}

void mtfs_stm32n6_aton_osal_inference_end(void)
{
    uint32_t notified, consumed;
    ER status;
    inference_active = 0U;
    status = consume_event(TMO_POL);
    if (status >= E_OK) ++osal_diagnostics.consumed_notifications;
    else if (!kernel_is_timeout(status))
        osal_diagnostics.last_kernel_error = status;
    notified = osal_diagnostics.irq_notifications -
        inference_notification_start;
    consumed = osal_diagnostics.consumed_notifications -
        inference_consumed_start;
    if (notified > consumed)
        osal_diagnostics.spurious_notifications += notified - consumed;
}

mtfs_stm32n6_event_wait_result_t mtfs_stm32n6_aton_osal_wait_event(
    uint32_t remaining_timeout_ms, int *immediate)
{
    ER status;
    TMO timeout;
    if (immediate == NULL || inference_active == 0U ||
        !mtfs_stm32n6_aton_osal_ready())
        return MTFS_STM32N6_EVENT_KERNEL_ERROR;
    *immediate = 0;
    status = consume_event(TMO_POL);
    if (status >= E_OK) {
        ++osal_diagnostics.consumed_notifications;
        *immediate = 1;
        return MTFS_STM32N6_EVENT_NOTIFIED;
    }
    if (!kernel_is_timeout(status)) {
        osal_diagnostics.last_kernel_error = status;
        return MTFS_STM32N6_EVENT_KERNEL_ERROR;
    }
    timeout = remaining_timeout_ms > (uint32_t)INT_MAX ?
        (TMO)INT_MAX : (TMO)remaining_timeout_ms;
    status = consume_event(timeout);
    if (status >= E_OK) {
        ++osal_diagnostics.consumed_notifications;
        return MTFS_STM32N6_EVENT_NOTIFIED;
    }
    if (kernel_is_timeout(status)) return MTFS_STM32N6_EVENT_TIMEOUT;
    osal_diagnostics.last_kernel_error = status;
    return MTFS_STM32N6_EVENT_KERNEL_ERROR;
}

mtfs_error_t mtfs_stm32n6_aton_osal_diagnostics_reset(void)
{
    if (inference_active != 0U) return MTFS_ERROR_INVALID_STATE;
    diagnostics_clear();
    return MTFS_OK;
}

void mtfs_stm32n6_aton_osal_diagnostics_get(
    mtfs_stm32n6_aton_osal_diagnostics_t *diagnostics)
{
    if (diagnostics == NULL) return;
    diagnostics->irq_notifications = osal_diagnostics.irq_notifications;
    diagnostics->consumed_notifications =
        osal_diagnostics.consumed_notifications;
    diagnostics->spurious_notifications =
        osal_diagnostics.spurious_notifications;
    diagnostics->late_notifications = osal_diagnostics.late_notifications;
    diagnostics->kernel_signal_errors =
        osal_diagnostics.kernel_signal_errors;
    diagnostics->last_kernel_error = osal_diagnostics.last_kernel_error;
}
