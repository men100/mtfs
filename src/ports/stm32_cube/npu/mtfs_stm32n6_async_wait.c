#include "mtfs_stm32n6_async_wait.h"

#include <limits.h>
#include <string.h>

static uint32_t elapsed_ms(uint32_t started, uint32_t now)
{
    return now - started;
}

static uint32_t remaining_ms(const mtfs_stm32n6_async_config_t *config,
    uint32_t started)
{
    uint32_t elapsed = elapsed_ms(started,
        config->clock_ms(config->context));
    return elapsed >= config->timeout_ms ? 0U : config->timeout_ms - elapsed;
}

static mtfs_error_t fail_run(const mtfs_stm32n6_async_config_t *config,
    mtfs_stm32n6_async_diagnostics_t *diagnostics, mtfs_error_t error)
{
    ++diagnostics->failed;
    if (config->recover != NULL) {
        config->recover(config->context);
        ++diagnostics->recoveries;
    }
    return error;
}

mtfs_error_t mtfs_stm32n6_async_run(
    const mtfs_stm32n6_async_config_t *config,
    mtfs_stm32n6_async_diagnostics_t *diagnostics)
{
    uint32_t started, consecutive_no_wfe = 0U;
    if (config == NULL || diagnostics == NULL || config->run_epoch == NULL ||
        config->clock_ms == NULL || config->wait_event == NULL ||
        config->timeout_ms == 0U || config->timeout_ms > (uint32_t)INT32_MAX ||
        config->consecutive_no_wfe_limit == 0U)
        return MTFS_ERROR_INVALID_ARGUMENT;
    (void)memset(diagnostics, 0, sizeof(*diagnostics));
    diagnostics->attempted = 1U;
    started = config->clock_ms(config->context);
    for (;;) {
        mtfs_stm32n6_async_state_t state;
        if (remaining_ms(config, started) == 0U) {
            ++diagnostics->timeouts;
            return fail_run(config, diagnostics, MTFS_ERROR_NOT_READY);
        }
        state = config->run_epoch(config->context);
        if (state == MTFS_STM32N6_ASYNC_DONE) {
            ++diagnostics->state_done;
            diagnostics->completed = 1U;
            return MTFS_OK;
        }
        if (state == MTFS_STM32N6_ASYNC_NO_WFE) {
            ++diagnostics->state_no_wfe;
            ++consecutive_no_wfe;
            if (consecutive_no_wfe > diagnostics->consecutive_no_wfe_max)
                diagnostics->consecutive_no_wfe_max = consecutive_no_wfe;
            if (consecutive_no_wfe >= config->consecutive_no_wfe_limit) {
                ++diagnostics->consecutive_no_wfe_limit_errors;
                return fail_run(config, diagnostics, MTFS_ERROR_NOT_READY);
            }
            continue;
        }
        if (state != MTFS_STM32N6_ASYNC_WFE) {
            ++diagnostics->state_other;
            return fail_run(config, diagnostics, MTFS_ERROR_IO);
        }
        ++diagnostics->state_wfe;
        consecutive_no_wfe = 0U;
        for (;;) {
            mtfs_stm32n6_event_wait_result_t wait_result;
            uint32_t remaining = remaining_ms(config, started);
            int immediate = 0;
            if (remaining == 0U) {
                ++diagnostics->timeouts;
                return fail_run(config, diagnostics, MTFS_ERROR_NOT_READY);
            }
            ++diagnostics->event_wait_starts;
            wait_result = config->wait_event(config->context, remaining,
                &immediate);
            if (wait_result == MTFS_STM32N6_EVENT_NOTIFIED) {
                if (immediate) ++diagnostics->immediate_event_completions;
                break;
            }
            if (wait_result == MTFS_STM32N6_EVENT_SPURIOUS) {
                ++diagnostics->spurious_notifications;
                continue;
            }
            if (wait_result == MTFS_STM32N6_EVENT_LATE) {
                ++diagnostics->late_notifications;
                continue;
            }
            if (wait_result == MTFS_STM32N6_EVENT_TIMEOUT) {
                if (remaining_ms(config, started) != 0U) continue;
                ++diagnostics->timeouts;
                return fail_run(config, diagnostics, MTFS_ERROR_NOT_READY);
            }
            ++diagnostics->kernel_wait_errors;
            return fail_run(config, diagnostics, MTFS_ERROR_IO);
        }
    }
}
