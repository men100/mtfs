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

#endif /* MTFS_STM32_SDMMC_WAIT_POLICY_H */
