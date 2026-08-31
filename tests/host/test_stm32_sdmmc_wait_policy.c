#include "test_stm32_sdmmc_wait_policy.h"

#include <stdint.h>

#include "mtfs_stm32_sdmmc_wait_policy.h"

int test_stm32_sdmmc_wait_policy(mtfs_test_t *test)
{
    const uint32_t started = UINT32_C(100);

    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_sdmmc_wait_policy_evaluate(
                started, started, started, 5000U) ==
                MTFS_STM32_SDMMC_WAIT_CONTINUE,
            "STM32 SDMMC wait initially busy-polls")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_sdmmc_wait_policy_evaluate(
                started, started, started +
                    MTFS_STM32_SDMMC_BUSY_POLL_MS - 1U,
                5000U) == MTFS_STM32_SDMMC_WAIT_CONTINUE,
            "STM32 SDMMC wait busy-polls before the bound")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_sdmmc_wait_policy_evaluate(
                started, started,
                started + MTFS_STM32_SDMMC_BUSY_POLL_MS, 5000U) ==
                MTFS_STM32_SDMMC_WAIT_BACKOFF,
            "STM32 SDMMC wait backs off at the busy-poll bound")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_sdmmc_wait_policy_evaluate(
                started, started, started + 5000U, 5000U) ==
                MTFS_STM32_SDMMC_WAIT_TIMEOUT,
            "STM32 SDMMC wait times out at the absolute deadline")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_sdmmc_wait_policy_evaluate(
                UINT32_MAX - 4U, UINT32_MAX - 4U, 5U, 100U) ==
                MTFS_STM32_SDMMC_WAIT_BACKOFF,
            "STM32 SDMMC busy-poll bound handles tick wrap")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_sdmmc_wait_policy_evaluate(
                UINT32_MAX - 4U, UINT32_MAX - 4U, 5U, 10U) ==
                MTFS_STM32_SDMMC_WAIT_TIMEOUT,
            "STM32 SDMMC absolute timeout handles tick wrap")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_sdmmc_ready_path_select(1, 1) ==
                MTFS_STM32_SDMMC_READY_PATH_IRQ &&
            mtfs_stm32_sdmmc_ready_path_select(0, 1) ==
                MTFS_STM32_SDMMC_READY_PATH_HYBRID &&
            mtfs_stm32_sdmmc_ready_path_select(1, 0) ==
                MTFS_STM32_SDMMC_READY_PATH_HYBRID,
            "STM32 SDMMC polling and non-write paths retain hybrid wait")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            !mtfs_stm32_sdmmc_ready_registers_accessible(1, 0) &&
            !mtfs_stm32_sdmmc_ready_registers_accessible(0, 1) &&
            mtfs_stm32_sdmmc_ready_registers_accessible(1, 1),
            "STM32 SDMMC ready cleanup waits for IDMA HAL initialization")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_sdmmc_ready_policy_evaluate(
                started, started, 5000U, 1, 0) ==
                MTFS_STM32_SDMMC_READY_COMPLETE,
            "STM32 SDMMC already-ready card completes without arming")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_sdmmc_ready_policy_evaluate(
                started, started, 5000U, 0, 1) ==
                MTFS_STM32_SDMMC_READY_ARM,
            "STM32 SDMMC busy card waits after IRQ arm")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_sdmmc_ready_policy_evaluate(
                started, started, 5000U, 0, 0) ==
                MTFS_STM32_SDMMC_READY_RECHECK,
            "STM32 SDMMC busy release around IRQ arm is rechecked")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_sdmmc_ready_remaining_ms(
                UINT32_MAX - 4U, 5U, 100U) == 90U &&
            mtfs_stm32_sdmmc_ready_remaining_ms(
                UINT32_MAX - 4U, 95U, 100U) == 0U,
            "STM32 SDMMC ready timeout remains absolute across tick wrap")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_sdmmc_ready_remaining_ms(
                started, started + 20U, 100U) == 80U &&
            mtfs_stm32_sdmmc_ready_remaining_ms(
                started, started + 60U, 100U) == 40U,
            "STM32 SDMMC rearm does not reset the ready timeout origin")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_sdmmc_ready_wakeup_evaluate(0, 1, 0, 0) ==
                MTFS_STM32_SDMMC_READY_WAKE_READY &&
            mtfs_stm32_sdmmc_ready_wakeup_evaluate(0, 0, 1, 0) ==
                MTFS_STM32_SDMMC_READY_WAKE_ERROR &&
            mtfs_stm32_sdmmc_ready_wakeup_evaluate(0, 0, 0, 1) ==
                MTFS_STM32_SDMMC_READY_WAKE_REMOVED &&
            mtfs_stm32_sdmmc_ready_wakeup_evaluate(1, 1, 0, 0) ==
                MTFS_STM32_SDMMC_READY_WAKE_KERNEL_ERROR,
            "STM32 SDMMC ready wakeups preserve event and kernel errors")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_sdmmc_ready_event_should_signal(1) &&
            !mtfs_stm32_sdmmc_ready_event_should_signal(0),
            "STM32 SDMMC cleanup ignores a late ready event")) {
        return 1;
    }
    return 0;
}
