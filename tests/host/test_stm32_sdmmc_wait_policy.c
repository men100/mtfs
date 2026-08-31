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
    return 0;
}
