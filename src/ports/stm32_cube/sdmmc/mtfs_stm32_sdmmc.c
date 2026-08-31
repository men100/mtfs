#include "mtfs_stm32_sdmmc.h"
#include "mtfs_stm32_sdmmc_wait_policy.h"
#include "../../../block/mtfs_block_diagnostics_internal.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "../common/mtfs_stm32_hal_timebase.h"

#define MTFS_STM32_SD_EVENT_RX       (UINT32_C(1) << 0)
#define MTFS_STM32_SD_EVENT_TX       (UINT32_C(1) << 1)
#define MTFS_STM32_SD_EVENT_ERROR    (UINT32_C(1) << 2)
#define MTFS_STM32_SD_EVENT_REMOVED  (UINT32_C(1) << 3)
#define MTFS_STM32_SD_EVENT_READY    (UINT32_C(1) << 4)

#if MTFS_ENABLE_DIAGNOSTICS
#define MTFS_ST_DIAGNOSTIC(...) do { __VA_ARGS__; } while (0)
#else
#define MTFS_ST_DIAGNOSTIC(...) do { } while (0)
#endif

#if MTFS_ENABLE_DIAGNOSTICS
static void mtfs_st_diagnostic_increment(uint32_t *counter)
{
    if (*counter != UINT32_MAX) {
        ++*counter;
    }
}
#endif

static mtfs_stm32_sdmmc_context_t *mtfs_stm32_sdmmc_irq_context;

static mtfs_error_t mtfs_stm32_sd_initialize(void *opaque);
static mtfs_error_t mtfs_stm32_sd_status(
    void *opaque, mtfs_block_status_t *status);
static mtfs_error_t mtfs_stm32_sd_read(
    void *opaque, void *buffer, mtfs_lba_t lba, uint32_t count);
static mtfs_error_t mtfs_stm32_sd_write(
    void *opaque, const void *buffer, mtfs_lba_t lba, uint32_t count);
static mtfs_error_t mtfs_stm32_sd_sync(void *opaque);
static mtfs_error_t mtfs_stm32_sd_geometry(
    void *opaque, mtfs_block_geometry_t *geometry);
static mtfs_error_t mtfs_stm32_sd_trim(
    void *opaque, mtfs_lba_t lba, mtfs_lba_t count);
static void mtfs_stm32_sd_abort(mtfs_stm32_sdmmc_context_t *context);

static void mtfs_stm32_sd_ready_irq_disarm(
    mtfs_stm32_sdmmc_context_t *context)
{
    if (!mtfs_stm32_sdmmc_ready_registers_accessible(
            context->config.use_idma, context->hal_initialized)) {
        context->ready_wait_active = 0U;
        return;
    }
    __HAL_SD_DISABLE_IT(context->config.hal_sd, SDMMC_IT_BUSYD0END);
    context->ready_wait_active = 0U;
    __HAL_SD_CLEAR_FLAG(context->config.hal_sd, SDMMC_FLAG_BUSYD0END);
}

static ER mtfs_stm32_sd_ready_irq_cleanup(
    mtfs_stm32_sdmmc_context_t *context)
{
    mtfs_stm32_sd_ready_irq_disarm(context);
    if (context->transfer_event_flag_id > 0) {
        return tk_clr_flg(context->transfer_event_flag_id,
            (UINT)~MTFS_STM32_SD_EVENT_READY);
    }
    return E_OK;
}

static ER mtfs_stm32_sd_clear_transfer_events(
    mtfs_stm32_sdmmc_context_t *context)
{
    const UINT transfer_events = MTFS_STM32_SD_EVENT_RX |
        MTFS_STM32_SD_EVENT_TX | MTFS_STM32_SD_EVENT_ERROR |
        MTFS_STM32_SD_EVENT_READY;
    return tk_clr_flg(context->transfer_event_flag_id,
        (UINT)~transfer_events);
}

static void mtfs_stm32_sd_invalidate_media(
    mtfs_stm32_sdmmc_context_t *context)
{
    context->initialized = 0U;
    (void)memset(&context->geometry, 0, sizeof(context->geometry));
}

static const mtfs_block_device_ops_t mtfs_stm32_sd_ops = {
    mtfs_stm32_sd_initialize,
    mtfs_stm32_sd_status,
    mtfs_stm32_sd_read,
    mtfs_stm32_sd_write,
    mtfs_stm32_sd_sync,
    mtfs_stm32_sd_geometry,
    mtfs_stm32_sd_trim
};

static mtfs_error_t mtfs_stm32_sd_set_error(
    mtfs_stm32_sdmmc_context_t *context, mtfs_error_t error)
{
    context->last_error = error;
    return error;
}

static int mtfs_stm32_sd_card_present(
    const mtfs_stm32_sdmmc_context_t *context)
{
    return (context->config.card_present == NULL) ||
        context->config.card_present(context->config.signal_context);
}

static int mtfs_stm32_sd_write_protected(
    const mtfs_stm32_sdmmc_context_t *context)
{
    return (context->config.write_protected != NULL) &&
        context->config.write_protected(context->config.signal_context);
}

static int mtfs_stm32_sd_platform_ready(
    const mtfs_stm32_sdmmc_context_t *context)
{
    return (context->config.idma_platform_ready == NULL) ||
        context->config.idma_platform_ready(context->config.signal_context);
}

static mtfs_error_t mtfs_stm32_sd_hal_error(
    mtfs_stm32_sdmmc_context_t *context, HAL_StatusTypeDef status)
{
    context->last_hal_status = status;
    context->last_hal_error = context->config.hal_sd->ErrorCode;
    if (!mtfs_stm32_sd_card_present(context)) {
        mtfs_stm32_sd_invalidate_media(context);
        return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NO_MEDIA);
    }
    if ((status == HAL_BUSY) || (status == HAL_TIMEOUT)) {
        return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_READY);
    }
    return mtfs_stm32_sd_set_error(context, MTFS_ERROR_IO);
}

static mtfs_error_t mtfs_stm32_sd_kernel_error(
    mtfs_stm32_sdmmc_context_t *context, ER error)
{
    context->last_kernel_error = error;
    if (error >= E_OK) {
        return mtfs_stm32_sd_set_error(context, MTFS_OK);
    }
    switch (MERCD(error)) {
    case MERCD(E_PAR):
    case MERCD(E_ID):
        return mtfs_stm32_sd_set_error(context, MTFS_ERROR_INVALID_ARGUMENT);
    case MERCD(E_NOSPT):
        return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_SUPPORTED);
    case MERCD(E_TMOUT):
    case MERCD(E_BUSY):
    case MERCD(E_OBJ):
    case MERCD(E_NOEXS):
        return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_READY);
    default:
        return mtfs_stm32_sd_set_error(context, MTFS_ERROR_IO);
    }
}

static mtfs_error_t mtfs_stm32_sd_lock(mtfs_stm32_sdmmc_context_t *context)
{
    if (context->access_mutex_id <= 0) {
        return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_READY);
    }
    return mtfs_stm32_sd_kernel_error(context,
        tk_loc_mtx(context->access_mutex_id, (TMO)context->config.io_timeout_ms));
}

static void mtfs_stm32_sd_unlock(mtfs_stm32_sdmmc_context_t *context)
{
    ER result = tk_unl_mtx(context->access_mutex_id);
    if (result < E_OK) {
        context->last_kernel_error = result;
        context->last_error = MTFS_ERROR_IO;
    }
}

static mtfs_error_t mtfs_stm32_sd_wait_transfer(
    mtfs_stm32_sdmmc_context_t *context)
{
    uint32_t started = HAL_GetTick();
    uint32_t busy_poll_started = started;

    /*
     * Most cards return to TRANSFER within the initial bounded poll, avoiding
     * the 10 ms kernel-tick rounding of a 1 ms delay on the normal path.  Once
     * that bound is exceeded, check the card once per task backoff so a busy
     * or faulty card cannot monopolize the CPU until the absolute timeout.
     */
    for (;;) {
        uint32_t now;
        mtfs_stm32_sdmmc_wait_action_t action;
        ER delay_result;

        if (!mtfs_stm32_sd_card_present(context)) {
            mtfs_stm32_sd_invalidate_media(context);
            return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NO_MEDIA);
        }
        if (context->media_removal_pending) {
            return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_READY);
        }
        if (HAL_SD_GetCardState(context->config.hal_sd) == HAL_SD_CARD_TRANSFER) {
            return mtfs_stm32_sd_set_error(context, MTFS_OK);
        }

        now = HAL_GetTick();
        action = mtfs_stm32_sdmmc_wait_policy_evaluate(
            started, busy_poll_started, now,
            context->config.transfer_timeout_ms);
        if (action == MTFS_STM32_SDMMC_WAIT_TIMEOUT) {
            break;
        }
        if (action == MTFS_STM32_SDMMC_WAIT_BACKOFF) {
            delay_result = tk_dly_tsk(1U);
            if (delay_result < E_OK) {
                return mtfs_stm32_sd_kernel_error(context, delay_result);
            }
        }
    }

    MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
        &context->diagnostics.card_state_timeouts));
    context->last_hal_error = context->config.hal_sd->ErrorCode;
    return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_READY);
}

static mtfs_error_t mtfs_stm32_sd_wait_ready_irq(
    mtfs_stm32_sdmmc_context_t *context)
{
    const uint32_t started = HAL_GetTick();
    uint32_t busy_poll_started = started;
    int fallback_recorded = 0;

    MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
        &context->diagnostics.ready_sequences));
    for (;;) {
        HAL_SD_CardStateTypeDef card_state;
        mtfs_stm32_sdmmc_ready_action_t action;
        uint32_t now;
        uint32_t remaining;
        UINT events = 0U;
        ER kernel_result;
        mtfs_stm32_sdmmc_ready_wakeup_t wakeup;

        if (!mtfs_stm32_sd_card_present(context)) {
            mtfs_stm32_sd_invalidate_media(context);
            (void)mtfs_stm32_sd_ready_irq_cleanup(context);
            return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NO_MEDIA);
        }
        if (context->media_removal_pending) {
            (void)mtfs_stm32_sd_ready_irq_cleanup(context);
            return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_READY);
        }
        if ((context->transfer_hal_error != HAL_SD_ERROR_NONE) ||
            (context->config.hal_sd->ErrorCode != HAL_SD_ERROR_NONE)) {
            (void)mtfs_stm32_sd_ready_irq_cleanup(context);
            context->last_hal_error = (context->transfer_hal_error !=
                HAL_SD_ERROR_NONE) ? context->transfer_hal_error
                                   : context->config.hal_sd->ErrorCode;
            return mtfs_stm32_sd_set_error(context, MTFS_ERROR_IO);
        }

        if (__HAL_SD_GET_FLAG(context->config.hal_sd,
                SDMMC_FLAG_BUSYD0) == RESET) {
            card_state = HAL_SD_GetCardState(context->config.hal_sd);
        } else {
            card_state = HAL_SD_CARD_PROGRAMMING;
        }
        if (context->config.hal_sd->ErrorCode != HAL_SD_ERROR_NONE) {
            (void)mtfs_stm32_sd_ready_irq_cleanup(context);
            context->last_hal_error = context->config.hal_sd->ErrorCode;
            return mtfs_stm32_sd_set_error(context, MTFS_ERROR_IO);
        }
        now = HAL_GetTick();
        action = mtfs_stm32_sdmmc_ready_policy_evaluate(
            started, now, context->config.transfer_timeout_ms,
            card_state == HAL_SD_CARD_TRANSFER,
            __HAL_SD_GET_FLAG(context->config.hal_sd,
                SDMMC_FLAG_BUSYD0) != RESET);
        if (action == MTFS_STM32_SDMMC_READY_COMPLETE) {
            kernel_result = mtfs_stm32_sd_ready_irq_cleanup(context);
            if (kernel_result < E_OK) {
                return mtfs_stm32_sd_kernel_error(context, kernel_result);
            }
            return mtfs_stm32_sd_set_error(context, MTFS_OK);
        }
        if (action == MTFS_STM32_SDMMC_READY_TIMEOUT) {
            (void)mtfs_stm32_sd_ready_irq_cleanup(context);
            MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
                &context->diagnostics.ready_wait_timeouts));
            MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
                &context->diagnostics.card_state_timeouts));
            context->last_hal_error = context->config.hal_sd->ErrorCode;
            return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_READY);
        }

        kernel_result = tk_clr_flg(context->transfer_event_flag_id,
            (UINT)~MTFS_STM32_SD_EVENT_READY);
        if (kernel_result < E_OK) {
            mtfs_stm32_sd_ready_irq_disarm(context);
            return mtfs_stm32_sd_kernel_error(context, kernel_result);
        }
        __HAL_SD_DISABLE_IT(context->config.hal_sd, SDMMC_IT_BUSYD0END);
        __HAL_SD_CLEAR_FLAG(context->config.hal_sd, SDMMC_FLAG_BUSYD0END);
        context->ready_wait_active = 1U;
        __HAL_SD_ENABLE_IT(context->config.hal_sd, SDMMC_IT_BUSYD0END);
        __DSB();

        if (!mtfs_stm32_sd_card_present(context)) {
            mtfs_stm32_sd_invalidate_media(context);
            (void)mtfs_stm32_sd_ready_irq_cleanup(context);
            return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NO_MEDIA);
        }
        if (context->media_removal_pending) {
            (void)mtfs_stm32_sd_ready_irq_cleanup(context);
            return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_READY);
        }

        now = HAL_GetTick();
        action = mtfs_stm32_sdmmc_ready_policy_evaluate(
            started, now, context->config.transfer_timeout_ms, 0,
            __HAL_SD_GET_FLAG(context->config.hal_sd,
                SDMMC_FLAG_BUSYD0) != RESET);
        if (action != MTFS_STM32_SDMMC_READY_ARM) {
            kernel_result = mtfs_stm32_sd_ready_irq_cleanup(context);
            if (kernel_result < E_OK) {
                return mtfs_stm32_sd_kernel_error(context, kernel_result);
            }
            if (action == MTFS_STM32_SDMMC_READY_TIMEOUT) {
                MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
                    &context->diagnostics.ready_wait_timeouts));
                MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
                    &context->diagnostics.card_state_timeouts));
                return mtfs_stm32_sd_set_error(
                    context, MTFS_ERROR_NOT_READY);
            }
            if ((now - busy_poll_started) >=
                MTFS_STM32_SDMMC_BUSY_POLL_MS) {
                if (!fallback_recorded) {
                    MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
                        &context->diagnostics.ready_hybrid_fallbacks));
                    fallback_recorded = 1;
                }
                kernel_result = tk_dly_tsk(1U);
                if (kernel_result < E_OK) {
                    return mtfs_stm32_sd_kernel_error(
                        context, kernel_result);
                }
                busy_poll_started = HAL_GetTick();
            }
            continue;
        }

        remaining = mtfs_stm32_sdmmc_ready_remaining_ms(started, now,
            context->config.transfer_timeout_ms);
        MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
            &context->diagnostics.ready_event_waits));
        kernel_result = tk_wai_flg(context->transfer_event_flag_id,
            MTFS_STM32_SD_EVENT_READY | MTFS_STM32_SD_EVENT_ERROR |
                MTFS_STM32_SD_EVENT_REMOVED,
            TWF_ORW | TWF_BITCLR, &events,
            (TMO)((remaining > (uint32_t)INT_MAX) ? INT_MAX : remaining));
        {
            ER cleanup_result = mtfs_stm32_sd_ready_irq_cleanup(context);
            if ((kernel_result >= E_OK) && (cleanup_result < E_OK)) {
                kernel_result = cleanup_result;
            }
        }
        wakeup = mtfs_stm32_sdmmc_ready_wakeup_evaluate(
            kernel_result < E_OK,
            (events & MTFS_STM32_SD_EVENT_READY) != 0U,
            (events & MTFS_STM32_SD_EVENT_ERROR) != 0U,
            (events & MTFS_STM32_SD_EVENT_REMOVED) != 0U);
        if (wakeup == MTFS_STM32_SDMMC_READY_WAKE_KERNEL_ERROR) {
            if (MERCD(kernel_result) == MERCD(E_TMOUT)) {
                if (mtfs_stm32_sdmmc_ready_remaining_ms(started,
                        HAL_GetTick(),
                        context->config.transfer_timeout_ms) != 0U) {
                    continue;
                }
                MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
                    &context->diagnostics.ready_wait_timeouts));
                MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
                    &context->diagnostics.card_state_timeouts));
            }
            return mtfs_stm32_sd_kernel_error(context, kernel_result);
        }
        if (wakeup == MTFS_STM32_SDMMC_READY_WAKE_REMOVED) {
            MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
                &context->diagnostics.media_wait_wakeups));
            mtfs_stm32_sd_invalidate_media(context);
            return mtfs_stm32_sd_set_error(context,
                mtfs_stm32_sd_card_present(context)
                    ? MTFS_ERROR_NOT_READY : MTFS_ERROR_NO_MEDIA);
        }
        if (wakeup == MTFS_STM32_SDMMC_READY_WAKE_ERROR) {
            context->last_hal_error = context->transfer_hal_error;
            return mtfs_stm32_sd_set_error(context, MTFS_ERROR_IO);
        }
        if (wakeup != MTFS_STM32_SDMMC_READY_WAKE_READY) {
            return mtfs_stm32_sd_set_error(context, MTFS_ERROR_IO);
        }
        MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
            &context->diagnostics.ready_event_wakeups));
    }
}

static void mtfs_stm32_sd_record_start(
    mtfs_stm32_sdmmc_context_t *context, int write, uint32_t blocks)
{
    MTFS_ST_DIAGNOSTIC(if (write) {
        if (blocks == 1U) {
            mtfs_st_diagnostic_increment(
                &context->diagnostics.write_single_starts);
        } else {
            mtfs_st_diagnostic_increment(
                &context->diagnostics.write_multi_starts);
        }
        if (blocks > context->diagnostics.write_max_blocks) {
            context->diagnostics.write_max_blocks = blocks;
        }
    } else {
        if (blocks == 1U) {
            mtfs_st_diagnostic_increment(
                &context->diagnostics.read_single_starts);
        } else {
            mtfs_st_diagnostic_increment(
                &context->diagnostics.read_multi_starts);
        }
        if (blocks > context->diagnostics.read_max_blocks) {
            context->diagnostics.read_max_blocks = blocks;
        }
    });
}

static mtfs_error_t mtfs_stm32_sd_polling_read(
    mtfs_stm32_sdmmc_context_t *context, void *buffer,
    uint32_t block, uint32_t count)
{
    uint8_t *cursor = buffer;
    uint32_t index;
    mtfs_error_t result = MTFS_OK;

    /* Avoid the FW_N6 V1.3.0 CMD18/CMD12 polling path. */
    for (index = 0U; index < count; ++index) {
        context->last_hal_status = HAL_SD_ReadBlocks(context->config.hal_sd,
            cursor, block + index, 1U, context->config.io_timeout_ms);
        if (context->last_hal_status != HAL_OK) {
            result = mtfs_stm32_sd_hal_error(
                context, context->last_hal_status);
            break;
        }
        mtfs_stm32_sd_record_start(context, 0, 1U);
        result = mtfs_stm32_sd_wait_transfer(context);
        if (result != MTFS_OK) {
            break;
        }
        cursor += MTFS_STM32_SDMMC_SECTOR_SIZE;
    }
    if (result != MTFS_OK) {
        mtfs_stm32_sd_abort(context);
    }
    return result;
}

static mtfs_error_t mtfs_stm32_sd_polling_write(
    mtfs_stm32_sdmmc_context_t *context, const void *buffer,
    uint32_t block, uint32_t count)
{
    const uint8_t *cursor = buffer;
    uint32_t index;
    mtfs_error_t result = MTFS_OK;

    /* Keep the fallback symmetric and independent of multi-block commands. */
    for (index = 0U; index < count; ++index) {
        context->last_hal_status = HAL_SD_WriteBlocks(context->config.hal_sd,
            cursor, block + index, 1U, context->config.io_timeout_ms);
        if (context->last_hal_status != HAL_OK) {
            result = mtfs_stm32_sd_hal_error(
                context, context->last_hal_status);
            break;
        }
        mtfs_stm32_sd_record_start(context, 1, 1U);
        result = mtfs_stm32_sd_wait_transfer(context);
        if (result != MTFS_OK) {
            break;
        }
        cursor += MTFS_STM32_SDMMC_SECTOR_SIZE;
    }
    if (result != MTFS_OK) {
        mtfs_stm32_sd_abort(context);
    }
    return result;
}

static void mtfs_stm32_sd_abort(mtfs_stm32_sdmmc_context_t *context)
{
    uint32_t transfer_error = context->transfer_hal_error;

    mtfs_stm32_sd_ready_irq_disarm(context);
    context->transfer_active = 0U;
    context->last_hal_status = HAL_SD_Abort(context->config.hal_sd);
    context->last_hal_error = (transfer_error != HAL_SD_ERROR_NONE)
        ? transfer_error : context->config.hal_sd->ErrorCode;
    mtfs_stm32_sd_invalidate_media(context);
    MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
        &context->diagnostics.aborts));
    if (context->transfer_event_flag_id > 0) {
        (void)tk_clr_flg(context->transfer_event_flag_id, 0U);
    }
}

static mtfs_error_t mtfs_stm32_sd_wait_event(
    mtfs_stm32_sdmmc_context_t *context, UINT completion_event)
{
    UINT events = 0U;
    ER result = tk_wai_flg(context->transfer_event_flag_id,
        completion_event | MTFS_STM32_SD_EVENT_ERROR |
            MTFS_STM32_SD_EVENT_REMOVED,
        TWF_ORW | TWF_BITCLR, &events, (TMO)context->config.io_timeout_ms);

    context->transfer_active = 0U;
    if (result < E_OK) {
        if (MERCD(result) == MERCD(E_TMOUT)) {
            MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
                &context->diagnostics.completion_timeouts));
        }
        return mtfs_stm32_sd_kernel_error(context, result);
    }
    if ((events & MTFS_STM32_SD_EVENT_REMOVED) != 0U) {
        MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
            &context->diagnostics.media_wait_wakeups));
        mtfs_stm32_sd_invalidate_media(context);
        return mtfs_stm32_sd_set_error(context,
            mtfs_stm32_sd_card_present(context)
                ? MTFS_ERROR_NOT_READY : MTFS_ERROR_NO_MEDIA);
    }
    if ((events & MTFS_STM32_SD_EVENT_ERROR) != 0U) {
        context->last_hal_error = context->transfer_hal_error;
        return mtfs_stm32_sd_set_error(context, MTFS_ERROR_IO);
    }
    if ((events & completion_event) == 0U) {
        return mtfs_stm32_sd_set_error(context, MTFS_ERROR_IO);
    }
    if (mtfs_stm32_sdmmc_ready_path_select(
            context->config.use_idma,
            completion_event == MTFS_STM32_SD_EVENT_TX) ==
        MTFS_STM32_SDMMC_READY_PATH_IRQ) {
        return mtfs_stm32_sd_wait_ready_irq(context);
    }
    return mtfs_stm32_sd_wait_transfer(context);
}

static mtfs_error_t mtfs_stm32_sd_idma_read(
    mtfs_stm32_sdmmc_context_t *context, uint8_t *buffer,
    uint32_t block, uint32_t count)
{
    while (count != 0U) {
        uint32_t blocks = (count > MTFS_STM32_SDMMC_BOUNCE_SECTORS)
            ? MTFS_STM32_SDMMC_BOUNCE_SECTORS : count;
        uint32_t size = blocks * MTFS_STM32_SDMMC_SECTOR_SIZE;
        mtfs_error_t result;

        SCB_CleanInvalidateDCache_by_Addr(
            (uint32_t *)(void *)context->bounce_buffer, (int32_t)size);
        __DSB();
        context->transfer_hal_error = HAL_SD_ERROR_NONE;
        result = mtfs_stm32_sd_kernel_error(context,
            mtfs_stm32_sd_clear_transfer_events(context));
        if (result != MTFS_OK) {
            return result;
        }
        if (!mtfs_stm32_sd_card_present(context)) {
            mtfs_stm32_sd_invalidate_media(context);
            return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NO_MEDIA);
        }
        if (context->media_removal_pending) {
            return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_READY);
        }
        context->transfer_active = 1U;
        context->last_hal_status = HAL_SD_ReadBlocks_DMA(context->config.hal_sd,
            context->bounce_buffer, block, blocks);
        if (context->last_hal_status != HAL_OK) {
            HAL_StatusTypeDef start_status = context->last_hal_status;
            mtfs_stm32_sd_abort(context);
            return mtfs_stm32_sd_hal_error(context, start_status);
        }
        mtfs_stm32_sd_record_start(context, 0, blocks);
        result = mtfs_stm32_sd_wait_event(context, MTFS_STM32_SD_EVENT_RX);
        if (result != MTFS_OK) {
            mtfs_stm32_sd_abort(context);
            return result;
        }
        SCB_InvalidateDCache_by_Addr(context->bounce_buffer, (int32_t)size);
        __DSB();
        (void)memcpy(buffer, context->bounce_buffer, size);
        buffer += size;
        block += blocks;
        count -= blocks;
    }
    return mtfs_stm32_sd_set_error(context, MTFS_OK);
}

static mtfs_error_t mtfs_stm32_sd_idma_write(
    mtfs_stm32_sdmmc_context_t *context, const uint8_t *buffer,
    uint32_t block, uint32_t count)
{
    while (count != 0U) {
        uint32_t blocks = (count > MTFS_STM32_SDMMC_BOUNCE_SECTORS)
            ? MTFS_STM32_SDMMC_BOUNCE_SECTORS : count;
        uint32_t size = blocks * MTFS_STM32_SDMMC_SECTOR_SIZE;
        mtfs_error_t result;

        (void)memcpy(context->bounce_buffer, buffer, size);
        SCB_CleanDCache_by_Addr(
            (uint32_t *)(void *)context->bounce_buffer, (int32_t)size);
        __DSB();
        context->transfer_hal_error = HAL_SD_ERROR_NONE;
        result = mtfs_stm32_sd_kernel_error(context,
            mtfs_stm32_sd_clear_transfer_events(context));
        if (result != MTFS_OK) {
            return result;
        }
        if (!mtfs_stm32_sd_card_present(context)) {
            mtfs_stm32_sd_invalidate_media(context);
            return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NO_MEDIA);
        }
        if (context->media_removal_pending) {
            return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_READY);
        }
        context->transfer_active = 1U;
        context->last_hal_status = HAL_SD_WriteBlocks_DMA(context->config.hal_sd,
            context->bounce_buffer, block, blocks);
        if (context->last_hal_status != HAL_OK) {
            HAL_StatusTypeDef start_status = context->last_hal_status;
            mtfs_stm32_sd_abort(context);
            return mtfs_stm32_sd_hal_error(context, start_status);
        }
        mtfs_stm32_sd_record_start(context, 1, blocks);
        result = mtfs_stm32_sd_wait_event(context, MTFS_STM32_SD_EVENT_TX);
        if (result != MTFS_OK) {
            mtfs_stm32_sd_abort(context);
            return result;
        }
        buffer += size;
        block += blocks;
        count -= blocks;
    }
    return mtfs_stm32_sd_set_error(context, MTFS_OK);
}

static mtfs_error_t mtfs_stm32_sd_validate_transfer(
    mtfs_stm32_sdmmc_context_t *context, mtfs_lba_t lba, uint32_t count,
    uint32_t *block)
{
    if (count == 0U) {
        return mtfs_stm32_sd_set_error(context, MTFS_ERROR_INVALID_ARGUMENT);
    }
    if (!mtfs_stm32_sd_card_present(context)) {
        mtfs_stm32_sd_invalidate_media(context);
        return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NO_MEDIA);
    }
    if (context->media_removal_pending) {
        return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_READY);
    }
    if (!context->initialized) {
        return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_READY);
    }
    if ((lba > UINT32_MAX) || (lba >= context->geometry.sector_count) ||
        ((mtfs_lba_t)count > (context->geometry.sector_count - lba))) {
        return mtfs_stm32_sd_set_error(context, MTFS_ERROR_OUT_OF_RANGE);
    }
    *block = (uint32_t)lba;
    return MTFS_OK;
}

static void mtfs_stm32_sd_irq_handler(UINT interrupt_number)
{
    mtfs_stm32_sdmmc_context_t *context = mtfs_stm32_sdmmc_irq_context;
    uint32_t status;
    uint32_t interrupt_mask;
    (void)interrupt_number;
    if ((context == NULL) || !context->irq_registered) {
        return;
    }
    MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
        &context->diagnostics.irq_entries));
    status = context->config.hal_sd->Instance->STA;
    interrupt_mask = context->config.hal_sd->Instance->MASK;
    if (((status & SDMMC_FLAG_BUSYD0END) != 0U) &&
        ((interrupt_mask & SDMMC_IT_BUSYD0END) != 0U)) {
        __HAL_SD_DISABLE_IT(context->config.hal_sd, SDMMC_IT_BUSYD0END);
        __HAL_SD_CLEAR_FLAG(context->config.hal_sd, SDMMC_FLAG_BUSYD0END);
        MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
            &context->diagnostics.busyd0end_irqs));
        if (mtfs_stm32_sdmmc_ready_event_should_signal(
                context->ready_wait_active) &&
            (context->transfer_event_flag_id > 0)) {
            (void)tk_set_flg(context->transfer_event_flag_id,
                MTFS_STM32_SD_EVENT_READY);
        }
    }
    HAL_SD_IRQHandler(context->config.hal_sd);
}

void HAL_SD_RxCpltCallback(SD_HandleTypeDef *hal_sd)
{
    mtfs_stm32_sdmmc_context_t *context = mtfs_stm32_sdmmc_irq_context;
    if ((context != NULL) && (hal_sd == context->config.hal_sd) &&
        context->transfer_active && (context->transfer_event_flag_id > 0)) {
        MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
            &context->diagnostics.rx_complete_callbacks));
        (void)tk_set_flg(context->transfer_event_flag_id,
            MTFS_STM32_SD_EVENT_RX);
    }
}

void HAL_SD_TxCpltCallback(SD_HandleTypeDef *hal_sd)
{
    mtfs_stm32_sdmmc_context_t *context = mtfs_stm32_sdmmc_irq_context;
    if ((context != NULL) && (hal_sd == context->config.hal_sd) &&
        context->transfer_active && (context->transfer_event_flag_id > 0)) {
        MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
            &context->diagnostics.tx_complete_callbacks));
        (void)tk_set_flg(context->transfer_event_flag_id,
            MTFS_STM32_SD_EVENT_TX);
    }
}

void HAL_SD_ErrorCallback(SD_HandleTypeDef *hal_sd)
{
    mtfs_stm32_sdmmc_context_t *context = mtfs_stm32_sdmmc_irq_context;
    if ((context != NULL) && (hal_sd == context->config.hal_sd)) {
        context->transfer_hal_error = hal_sd->ErrorCode;
        context->last_hal_error = hal_sd->ErrorCode;
        MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
            &context->diagnostics.error_callbacks));
        if ((context->transfer_active || context->ready_wait_active) &&
            (context->transfer_event_flag_id > 0)) {
            (void)tk_set_flg(context->transfer_event_flag_id,
                MTFS_STM32_SD_EVENT_ERROR);
        }
    }
}

mtfs_error_t mtfs_stm32_sdmmc_context_init(
    mtfs_stm32_sdmmc_context_t *context,
    const mtfs_stm32_sdmmc_config_t *config)
{
    T_CMTX mutex = {0};
    T_CFLG event_flag = {0};
    T_DINT interrupt = {0};
    mtfs_error_t result = MTFS_ERROR_IO;

    if ((context == NULL) || (config == NULL) || (config->hal_sd == NULL) ||
        (config->use_idma > 1U) || (config->manage_hal_timebase > 1U)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    if (config->use_idma && (mtfs_stm32_sdmmc_irq_context != NULL)) {
        return MTFS_ERROR_ALREADY_EXISTS;
    }

    (void)memset(context, 0, sizeof(*context));
    context->config = *config;
    if (context->config.io_timeout_ms == 0U) {
        context->config.io_timeout_ms = MTFS_STM32_SDMMC_DEFAULT_TIMEOUT_MS;
    }
    if (context->config.transfer_timeout_ms == 0U) {
        context->config.transfer_timeout_ms = MTFS_STM32_SDMMC_DEFAULT_TIMEOUT_MS;
    }
    context->block_device.ops = &mtfs_stm32_sd_ops;
    context->block_device.context = context;
#if MTFS_ENABLE_DIAGNOSTICS
    context->diagnostics.api_version =
        MTFS_STM32_SDMMC_DIAGNOSTICS_API_VERSION;
    context->diagnostics.struct_size = (uint16_t)sizeof(context->diagnostics);
    context->diagnostics.validity_mask =
        MTFS_STM32_SDMMC_DIAGNOSTICS_VALID_ALL;
    (void)mtfs_block_diagnostics_attach(
        &context->block_device, &context->block_diagnostics);
#endif

    if (context->config.manage_hal_timebase) {
        if (mtfs_stm32_hal_timebase_acquire() != HAL_OK) {
            goto fail;
        }
        context->timebase_acquired = 1U;
    }

    mutex.mtxatr = TA_INHERIT;
    context->access_mutex_id = tk_cre_mtx(&mutex);
    if (context->access_mutex_id <= 0) {
        context->last_kernel_error = context->access_mutex_id;
        goto fail;
    }

    if (context->config.use_idma) {
        event_flag.flgatr = TA_TFIFO;
        context->transfer_event_flag_id = tk_cre_flg(&event_flag);
        if (context->transfer_event_flag_id <= 0) {
            context->last_kernel_error = context->transfer_event_flag_id;
            goto fail;
        }
        interrupt.intatr = TA_HLNG;
        interrupt.inthdr = (FP)mtfs_stm32_sd_irq_handler;
        context->last_kernel_error = tk_def_int(
            (UINT)context->config.irq_number, &interrupt);
        if (context->last_kernel_error < E_OK) {
            goto fail;
        }
        context->irq_registered = 1U;
        mtfs_stm32_sdmmc_irq_context = context;
    }

    context->objects_ready = 1U;
    return mtfs_stm32_sd_set_error(context, MTFS_OK);

fail:
    if (context->irq_registered) {
        (void)tk_def_int((UINT)context->config.irq_number, NULL);
        context->irq_registered = 0U;
    }
    if (context->transfer_event_flag_id > 0) {
        (void)tk_del_flg(context->transfer_event_flag_id);
        context->transfer_event_flag_id = 0;
    }
    if (context->access_mutex_id > 0) {
        (void)tk_del_mtx(context->access_mutex_id);
        context->access_mutex_id = 0;
    }
    if (context->timebase_acquired) {
        (void)mtfs_stm32_hal_timebase_release();
        context->timebase_acquired = 0U;
    }
    return mtfs_stm32_sd_set_error(context, result);
}

mtfs_error_t mtfs_stm32_sdmmc_context_deinit(
    mtfs_stm32_sdmmc_context_t *context)
{
    mtfs_error_t result = MTFS_OK;

    if (context == NULL) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    if (!context->objects_ready) {
        return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_READY);
    }

    if (context->irq_registered) {
        mtfs_stm32_sd_ready_irq_disarm(context);
        HAL_NVIC_DisableIRQ(context->config.irq_number);
    }
    if (context->hal_initialized || context->transfer_active) {
        if (context->transfer_active) {
            mtfs_stm32_sd_abort(context);
        }
        /* Prevent an ISR from touching registers while MSP deinit gates them. */
        context->hal_initialized = 0U;
        context->last_hal_status = HAL_SD_DeInit(context->config.hal_sd);
        if (context->last_hal_status != HAL_OK) {
            result = MTFS_ERROR_IO;
        }
    }
    context->initialized = 0U;
    context->objects_ready = 0U;
    context->transfer_active = 0U;
    context->ready_wait_active = 0U;

    if (context->irq_registered) {
        if (tk_def_int((UINT)context->config.irq_number, NULL) < E_OK) {
            result = MTFS_ERROR_IO;
        }
        context->irq_registered = 0U;
    }
    if (mtfs_stm32_sdmmc_irq_context == context) {
        mtfs_stm32_sdmmc_irq_context = NULL;
    }
    if ((context->transfer_event_flag_id > 0) &&
        (tk_del_flg(context->transfer_event_flag_id) < E_OK)) {
        result = MTFS_ERROR_IO;
    }
    context->transfer_event_flag_id = 0;
    if ((context->access_mutex_id > 0) &&
        (tk_del_mtx(context->access_mutex_id) < E_OK)) {
        result = MTFS_ERROR_IO;
    }
    context->access_mutex_id = 0;
    if (context->timebase_acquired) {
        if (mtfs_stm32_hal_timebase_release() != HAL_OK) {
            result = MTFS_ERROR_IO;
        }
        context->timebase_acquired = 0U;
    }
    return mtfs_stm32_sd_set_error(context, result);
}

mtfs_block_device_t *mtfs_stm32_sdmmc_block_device(
    mtfs_stm32_sdmmc_context_t *context)
{
    return (context == NULL) ? NULL : &context->block_device;
}

mtfs_error_t mtfs_stm32_sdmmc_diagnostics_get(
    mtfs_stm32_sdmmc_context_t *context,
    mtfs_stm32_sdmmc_diagnostics_t *snapshot)
{
    mtfs_error_t result;
    int locked = 0;
    if ((context == NULL) || (snapshot == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
#if MTFS_ENABLE_DIAGNOSTICS
    if (context->diagnostics.api_version !=
        MTFS_STM32_SDMMC_DIAGNOSTICS_API_VERSION) {
        return MTFS_ERROR_NOT_READY;
    }
    if (context->access_mutex_id > 0) {
        result = mtfs_stm32_sd_lock(context);
        if (result != MTFS_OK) return result;
        locked = 1;
    }
    *snapshot = context->diagnostics;
    snapshot->last_hal_status = (int32_t)context->last_hal_status;
    snapshot->last_hal_error = context->last_hal_error;
    snapshot->transfer_hal_error = context->transfer_hal_error;
    snapshot->last_kernel_error = (int32_t)context->last_kernel_error;
    snapshot->last_error = (int32_t)context->last_error;
    snapshot->sector_count = context->geometry.sector_count;
    snapshot->sector_size = context->geometry.sector_size;
    snapshot->erase_block_size = context->geometry.erase_block_size;
    snapshot->bounce_buffer_size = MTFS_STM32_SDMMC_BOUNCE_SIZE;
    snapshot->cache_line_size = MTFS_STM32_SDMMC_CACHE_LINE_SIZE;
    snapshot->use_idma = context->config.use_idma;
    snapshot->initialized = context->initialized;
    snapshot->hal_initialized = context->hal_initialized;
    snapshot->transfer_active = context->transfer_active;
    if (locked) mtfs_stm32_sd_unlock(context);
    return MTFS_OK;
#else
    (void)result;
    (void)locked;
    return MTFS_ERROR_NOT_SUPPORTED;
#endif
}

mtfs_error_t mtfs_stm32_sdmmc_diagnostics_reset(
    mtfs_stm32_sdmmc_context_t *context)
{
    mtfs_error_t result;
    uint32_t epoch;
    uint32_t clkcr;
    int locked = 0;
    if (context == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
#if MTFS_ENABLE_DIAGNOSTICS
    if (context->diagnostics.api_version !=
        MTFS_STM32_SDMMC_DIAGNOSTICS_API_VERSION) {
        return MTFS_ERROR_NOT_READY;
    }
    if (context->access_mutex_id > 0) {
        result = mtfs_stm32_sd_lock(context);
        if (result != MTFS_OK) return result;
        locked = 1;
    }
    epoch = context->diagnostics.reset_epoch + 1U;
    clkcr = context->diagnostics.last_clkcr;
    (void)memset(&context->diagnostics, 0, sizeof(context->diagnostics));
    context->diagnostics.api_version =
        MTFS_STM32_SDMMC_DIAGNOSTICS_API_VERSION;
    context->diagnostics.struct_size = (uint16_t)sizeof(context->diagnostics);
    context->diagnostics.validity_mask =
        MTFS_STM32_SDMMC_DIAGNOSTICS_VALID_ALL;
    context->diagnostics.reset_epoch = epoch;
    context->diagnostics.last_clkcr = clkcr;
    if (locked) mtfs_stm32_sd_unlock(context);
    return MTFS_OK;
#else
    (void)result;
    (void)epoch;
    (void)clkcr;
    (void)locked;
    return MTFS_ERROR_NOT_SUPPORTED;
#endif
}

mtfs_error_t mtfs_stm32_sdmmc_media_changed_isr(
    mtfs_stm32_sdmmc_context_t *context, int present)
{
    if ((context == NULL) || !context->objects_ready ||
        (context->config.card_present == NULL)) {
        return MTFS_ERROR_NOT_READY;
    }
    if (present) {
        return MTFS_OK;
    }

    context->media_removal_pending = 1U;
    context->initialized = 0U;
    mtfs_stm32_sd_ready_irq_disarm(context);
    MTFS_ST_DIAGNOSTIC(mtfs_st_diagnostic_increment(
        &context->diagnostics.media_removal_notifications));
    if (context->transfer_event_flag_id > 0) {
        ER result = tk_set_flg(context->transfer_event_flag_id,
            MTFS_STM32_SD_EVENT_REMOVED);
        if (result < E_OK) {
            context->last_kernel_error = result;
            return MTFS_ERROR_IO;
        }
    }
    return MTFS_OK;
}

static mtfs_error_t mtfs_stm32_sd_initialize(void *opaque)
{
    mtfs_stm32_sdmmc_context_t *context = opaque;
    HAL_SD_CardInfoTypeDef card_info;
    mtfs_error_t result;

    if ((context == NULL) || !context->objects_ready) {
        return MTFS_ERROR_NOT_READY;
    }
    if (!mtfs_stm32_sd_card_present(context)) {
        mtfs_stm32_sd_invalidate_media(context);
        return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NO_MEDIA);
    }
    if (context->config.use_idma && !mtfs_stm32_sd_platform_ready(context)) {
        context->initialized = 0U;
        return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_READY);
    }
    result = mtfs_stm32_sd_lock(context);
    if (result != MTFS_OK) {
        return result;
    }

    /*
     * A raw removal hint may be produced by contact bounce during insertion.
     * Explicit initialize, under the I/O mutex and after a stable present
     * check, is the safe recovery boundary for clearing that stale hint.
    */
    context->media_removal_pending = 0U;
    if (context->transfer_event_flag_id > 0) {
        mtfs_stm32_sd_ready_irq_disarm(context);
        result = mtfs_stm32_sd_kernel_error(context,
            tk_clr_flg(context->transfer_event_flag_id, 0U));
        if (result != MTFS_OK) {
            goto done;
        }
    }
    mtfs_stm32_sd_invalidate_media(context);
    if (context->hal_initialized) {
        /* Prevent card-detect ISR register access during peripheral teardown. */
        context->hal_initialized = 0U;
        context->last_hal_status = HAL_SD_DeInit(context->config.hal_sd);
        if (context->last_hal_status != HAL_OK) {
            result = mtfs_stm32_sd_hal_error(context, context->last_hal_status);
            goto done;
        }
    }
    context->last_hal_status = HAL_SD_Init(context->config.hal_sd);
    /* HAL_SD_MspInit() has returned, so SDMMC registers are now accessible. */
    context->hal_initialized = 1U;
    if (context->last_hal_status != HAL_OK) {
        result = mtfs_stm32_sd_hal_error(context, context->last_hal_status);
        goto done;
    }
    mtfs_stm32_sd_ready_irq_disarm(context);
    /* Snapshot while the peripheral clock is known to be enabled. */
    MTFS_ST_DIAGNOSTIC(context->diagnostics.last_clkcr =
        context->config.hal_sd->Instance->CLKCR);
    result = mtfs_stm32_sd_wait_transfer(context);
    if (result != MTFS_OK) {
        goto failed_init;
    }
    context->last_hal_status = HAL_SD_GetCardInfo(
        context->config.hal_sd, &card_info);
    if (context->last_hal_status != HAL_OK) {
        result = mtfs_stm32_sd_hal_error(context, context->last_hal_status);
        goto failed_init;
    }
    if ((card_info.LogBlockSize != MTFS_STM32_SDMMC_SECTOR_SIZE) ||
        (card_info.LogBlockNbr == 0U)) {
        result = mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_SUPPORTED);
        goto failed_init;
    }
    context->geometry.sector_size = card_info.LogBlockSize;
    context->geometry.sector_count = card_info.LogBlockNbr;
    /* STM32 HAL does not expose reliable SD erase-unit geometry. */
    context->geometry.erase_block_size = 1U;
    context->media_removal_pending = 0U;
    context->initialized = 1U;
    /* Explicit initialize is the recovery boundary for stale transport state. */
    context->last_hal_status = HAL_OK;
    context->last_hal_error = HAL_SD_ERROR_NONE;
    context->transfer_hal_error = HAL_SD_ERROR_NONE;
    context->last_kernel_error = E_OK;
    result = mtfs_stm32_sd_set_error(context, MTFS_OK);
    goto done;

failed_init:
    mtfs_stm32_sd_ready_irq_disarm(context);
    context->hal_initialized = 0U;
    context->last_hal_status = HAL_SD_DeInit(context->config.hal_sd);
    context->initialized = 0U;
done:
    mtfs_stm32_sd_unlock(context);
    return result;
}

static mtfs_error_t mtfs_stm32_sd_status(
    void *opaque, mtfs_block_status_t *status)
{
    mtfs_stm32_sdmmc_context_t *context = opaque;
    mtfs_error_t result;

    if ((context == NULL) || (status == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    *status = 0U;
    result = mtfs_stm32_sd_lock(context);
    if (result != MTFS_OK) {
        return result;
    }
    if (!mtfs_stm32_sd_card_present(context)) {
        mtfs_stm32_sd_invalidate_media(context);
        result = mtfs_stm32_sd_set_error(context, MTFS_ERROR_NO_MEDIA);
        goto done;
    }
    *status |= MTFS_BLOCK_STATUS_MEDIA_PRESENT;
    if (mtfs_stm32_sd_write_protected(context)) {
        *status |= MTFS_BLOCK_STATUS_WRITE_PROTECTED;
    }
    if (!context->initialized || context->media_removal_pending) {
        result = mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_READY);
        goto done;
    }
    result = mtfs_stm32_sd_wait_transfer(context);
    if (result == MTFS_OK) {
        *status |= MTFS_BLOCK_STATUS_INITIALIZED;
    } else {
        context->initialized = 0U;
    }
done:
    mtfs_stm32_sd_unlock(context);
    return result;
}

static mtfs_error_t mtfs_stm32_sd_read(
    void *opaque, void *buffer, mtfs_lba_t lba, uint32_t count)
{
    mtfs_stm32_sdmmc_context_t *context = opaque;
    uint32_t block;
    mtfs_error_t result;

    if ((context == NULL) || (buffer == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    result = mtfs_stm32_sd_lock(context);
    if (result != MTFS_OK) {
        return result;
    }
    result = mtfs_stm32_sd_validate_transfer(context, lba, count, &block);
    if (result != MTFS_OK) {
        goto done;
    }
    if (context->config.use_idma) {
        result = mtfs_stm32_sd_idma_read(context, buffer, block, count);
    } else {
        result = mtfs_stm32_sd_polling_read(
            context, buffer, block, count);
    }
done:
    mtfs_stm32_sd_unlock(context);
    return result;
}

static mtfs_error_t mtfs_stm32_sd_write(
    void *opaque, const void *buffer, mtfs_lba_t lba, uint32_t count)
{
    mtfs_stm32_sdmmc_context_t *context = opaque;
    uint32_t block;
    mtfs_error_t result;

    if ((context == NULL) || (buffer == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    result = mtfs_stm32_sd_lock(context);
    if (result != MTFS_OK) {
        return result;
    }
    result = mtfs_stm32_sd_validate_transfer(context, lba, count, &block);
    if (result != MTFS_OK) {
        goto done;
    }
    if (mtfs_stm32_sd_write_protected(context)) {
        result = mtfs_stm32_sd_set_error(context, MTFS_ERROR_WRITE_PROTECTED);
        goto done;
    }
    if (context->config.use_idma) {
        result = mtfs_stm32_sd_idma_write(context, buffer, block, count);
    } else {
        result = mtfs_stm32_sd_polling_write(
            context, buffer, block, count);
    }
done:
    mtfs_stm32_sd_unlock(context);
    return result;
}

static mtfs_error_t mtfs_stm32_sd_sync(void *opaque)
{
    mtfs_stm32_sdmmc_context_t *context = opaque;
    mtfs_error_t result;

    if (context == NULL) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    result = mtfs_stm32_sd_lock(context);
    if (result != MTFS_OK) {
        return result;
    }
    if (!mtfs_stm32_sd_card_present(context)) {
        mtfs_stm32_sd_invalidate_media(context);
        result = mtfs_stm32_sd_set_error(context, MTFS_ERROR_NO_MEDIA);
    } else if (!context->initialized || context->media_removal_pending) {
        result = mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_READY);
    } else {
        result = mtfs_stm32_sd_wait_transfer(context);
        if (result != MTFS_OK) {
            mtfs_stm32_sd_abort(context);
        }
    }
    mtfs_stm32_sd_unlock(context);
    return result;
}

static mtfs_error_t mtfs_stm32_sd_geometry(
    void *opaque, mtfs_block_geometry_t *geometry)
{
    mtfs_stm32_sdmmc_context_t *context = opaque;
    mtfs_error_t result;

    if ((context == NULL) || (geometry == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    result = mtfs_stm32_sd_lock(context);
    if (result != MTFS_OK) {
        return result;
    }
    if (!mtfs_stm32_sd_card_present(context)) {
        mtfs_stm32_sd_invalidate_media(context);
        result = mtfs_stm32_sd_set_error(context, MTFS_ERROR_NO_MEDIA);
    } else if (!context->initialized || context->media_removal_pending) {
        result = mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_READY);
    } else {
        *geometry = context->geometry;
        result = mtfs_stm32_sd_set_error(context, MTFS_OK);
    }
    mtfs_stm32_sd_unlock(context);
    return result;
}

static mtfs_error_t mtfs_stm32_sd_trim(
    void *opaque, mtfs_lba_t lba, mtfs_lba_t count)
{
    mtfs_stm32_sdmmc_context_t *context = opaque;
    (void)lba;
    (void)count;
    if (context == NULL) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    return mtfs_stm32_sd_set_error(context, MTFS_ERROR_NOT_SUPPORTED);
}
