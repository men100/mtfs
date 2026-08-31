#ifndef MTFS_STM32_SDMMC_WAIT_POLICY_H
#define MTFS_STM32_SDMMC_WAIT_POLICY_H

#include <stdint.h>

/* Internal scheduler-safety policy; this is not a user-facing profile. */
#define MTFS_STM32_SDMMC_BUSY_POLL_MS (10U)

typedef enum mtfs_stm32_sdmmc_wait_action
{
    MTFS_STM32_SDMMC_WAIT_CONTINUE = 0,
    MTFS_STM32_SDMMC_WAIT_BACKOFF,
    MTFS_STM32_SDMMC_WAIT_TIMEOUT
} mtfs_stm32_sdmmc_wait_action_t;

typedef enum mtfs_stm32_sdmmc_ready_path
{
    MTFS_STM32_SDMMC_READY_PATH_HYBRID = 0,
    MTFS_STM32_SDMMC_READY_PATH_IRQ
} mtfs_stm32_sdmmc_ready_path_t;

typedef enum mtfs_stm32_sdmmc_ready_action
{
    MTFS_STM32_SDMMC_READY_COMPLETE = 0,
    MTFS_STM32_SDMMC_READY_ARM,
    MTFS_STM32_SDMMC_READY_RECHECK,
    MTFS_STM32_SDMMC_READY_TIMEOUT
} mtfs_stm32_sdmmc_ready_action_t;

typedef enum mtfs_stm32_sdmmc_ready_wakeup
{
    MTFS_STM32_SDMMC_READY_WAKE_READY = 0,
    MTFS_STM32_SDMMC_READY_WAKE_ERROR,
    MTFS_STM32_SDMMC_READY_WAKE_REMOVED,
    MTFS_STM32_SDMMC_READY_WAKE_KERNEL_ERROR,
    MTFS_STM32_SDMMC_READY_WAKE_UNEXPECTED
} mtfs_stm32_sdmmc_ready_wakeup_t;

static inline mtfs_stm32_sdmmc_wait_action_t
mtfs_stm32_sdmmc_wait_policy_evaluate(
    uint32_t started, uint32_t busy_poll_started, uint32_t now,
    uint32_t timeout_ms)
{
    /* Unsigned subtraction keeps both elapsed checks safe across tick wrap. */
    if ((now - started) >= timeout_ms) {
        return MTFS_STM32_SDMMC_WAIT_TIMEOUT;
    }
    if ((now - busy_poll_started) >= MTFS_STM32_SDMMC_BUSY_POLL_MS) {
        return MTFS_STM32_SDMMC_WAIT_BACKOFF;
    }
    return MTFS_STM32_SDMMC_WAIT_CONTINUE;
}

static inline mtfs_stm32_sdmmc_ready_path_t
mtfs_stm32_sdmmc_ready_path_select(int use_idma, int write_completed)
{
    return (use_idma && write_completed)
        ? MTFS_STM32_SDMMC_READY_PATH_IRQ
        : MTFS_STM32_SDMMC_READY_PATH_HYBRID;
}

static inline int mtfs_stm32_sdmmc_ready_registers_accessible(
    int use_idma, int hal_initialized)
{
    return use_idma && hal_initialized;
}

static inline uint32_t mtfs_stm32_sdmmc_ready_remaining_ms(
    uint32_t started, uint32_t now, uint32_t timeout_ms)
{
    uint32_t elapsed = now - started;
    return (elapsed >= timeout_ms) ? 0U : timeout_ms - elapsed;
}

static inline mtfs_stm32_sdmmc_ready_action_t
mtfs_stm32_sdmmc_ready_policy_evaluate(
    uint32_t started, uint32_t now, uint32_t timeout_ms,
    int card_transfer, int busy_after_arm)
{
    if (card_transfer) {
        return MTFS_STM32_SDMMC_READY_COMPLETE;
    }
    if (mtfs_stm32_sdmmc_ready_remaining_ms(
            started, now, timeout_ms) == 0U) {
        return MTFS_STM32_SDMMC_READY_TIMEOUT;
    }
    return busy_after_arm ? MTFS_STM32_SDMMC_READY_ARM
                          : MTFS_STM32_SDMMC_READY_RECHECK;
}

static inline mtfs_stm32_sdmmc_ready_wakeup_t
mtfs_stm32_sdmmc_ready_wakeup_evaluate(
    int kernel_error, int ready, int error, int removed)
{
    if (kernel_error) {
        return MTFS_STM32_SDMMC_READY_WAKE_KERNEL_ERROR;
    }
    if (removed) {
        return MTFS_STM32_SDMMC_READY_WAKE_REMOVED;
    }
    if (error) {
        return MTFS_STM32_SDMMC_READY_WAKE_ERROR;
    }
    if (ready) {
        return MTFS_STM32_SDMMC_READY_WAKE_READY;
    }
    return MTFS_STM32_SDMMC_READY_WAKE_UNEXPECTED;
}

static inline int mtfs_stm32_sdmmc_ready_event_should_signal(
    int ready_wait_active)
{
    return ready_wait_active != 0;
}

#endif /* MTFS_STM32_SDMMC_WAIT_POLICY_H */
