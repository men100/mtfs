#ifndef MTFS_STM32N6_ASYNC_WAIT_H
#define MTFS_STM32N6_ASYNC_WAIT_H

#include <stdint.h>

#include "../../../mtfs_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mtfs_stm32n6_async_state
{
    MTFS_STM32N6_ASYNC_NO_WFE = 0,
    MTFS_STM32N6_ASYNC_WFE = 1,
    MTFS_STM32N6_ASYNC_DONE = 2,
    MTFS_STM32N6_ASYNC_UNKNOWN = 3
} mtfs_stm32n6_async_state_t;

typedef enum mtfs_stm32n6_event_wait_result
{
    MTFS_STM32N6_EVENT_NOTIFIED = 0,
    MTFS_STM32N6_EVENT_TIMEOUT = 1,
    MTFS_STM32N6_EVENT_SPURIOUS = 2,
    MTFS_STM32N6_EVENT_LATE = 3,
    MTFS_STM32N6_EVENT_KERNEL_ERROR = 4
} mtfs_stm32n6_event_wait_result_t;

typedef mtfs_stm32n6_async_state_t (*mtfs_stm32n6_run_epoch_fn)(
    void *context);
typedef uint32_t (*mtfs_stm32n6_async_clock_fn)(void *context);
typedef mtfs_stm32n6_event_wait_result_t (*mtfs_stm32n6_event_wait_fn)(
    void *context, uint32_t remaining_timeout_ms, int *immediate);
typedef void (*mtfs_stm32n6_async_recover_fn)(void *context);

typedef struct mtfs_stm32n6_async_config
{
    void *context;
    mtfs_stm32n6_run_epoch_fn run_epoch;
    mtfs_stm32n6_async_clock_fn clock_ms;
    mtfs_stm32n6_event_wait_fn wait_event;
    mtfs_stm32n6_async_recover_fn recover;
    uint32_t timeout_ms;
    uint32_t consecutive_no_wfe_limit;
} mtfs_stm32n6_async_config_t;

typedef struct mtfs_stm32n6_async_diagnostics
{
    uint32_t attempted;
    uint32_t completed;
    uint32_t failed;
    uint32_t state_no_wfe;
    uint32_t state_wfe;
    uint32_t state_done;
    uint32_t state_other;
    uint32_t event_wait_starts;
    uint32_t immediate_event_completions;
    uint32_t spurious_notifications;
    uint32_t late_notifications;
    uint32_t timeouts;
    uint32_t kernel_wait_errors;
    uint32_t consecutive_no_wfe_max;
    uint32_t consecutive_no_wfe_limit_errors;
    uint32_t recoveries;
} mtfs_stm32n6_async_diagnostics_t;

mtfs_error_t mtfs_stm32n6_async_run(
    const mtfs_stm32n6_async_config_t *config,
    mtfs_stm32n6_async_diagnostics_t *diagnostics);

#ifdef __cplusplus
}
#endif

#endif
