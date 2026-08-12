#include <stddef.h>
#include <string.h>

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "stm32n6xx.h"

#include "mtfs_block_device.h"
#include "mtfs_block_registry.h"
#include "mtfs_stm32_sdmmc.h"
#include "mtfs_test.h"
#include "test_fatfs_roundtrip.h"
#include "mtfs_stm32n6570_dk_platform.h"
#include "mtfs_target_concurrent.h"

#define MTFS_TEST_PROFILE_SMOKE   (1)
#define MTFS_TEST_PROFILE_NORMAL  (2)
#define MTFS_TEST_PROFILE_STRESS  (3)

#ifndef MTFS_STM32N6570_TEST_PROFILE
#define MTFS_STM32N6570_TEST_PROFILE MTFS_TEST_PROFILE_NORMAL
#endif

#if MTFS_STM32N6570_TEST_PROFILE == MTFS_TEST_PROFILE_SMOKE
#define MTFS_STM32N6570_TEST_ROUNDS       (1U)
#define MTFS_STM32N6570_TEST_PROFILE_NAME "smoke"
#elif MTFS_STM32N6570_TEST_PROFILE == MTFS_TEST_PROFILE_NORMAL
#define MTFS_STM32N6570_TEST_ROUNDS       (10U)
#define MTFS_STM32N6570_TEST_PROFILE_NAME "normal"
#elif MTFS_STM32N6570_TEST_PROFILE == MTFS_TEST_PROFILE_STRESS
#define MTFS_STM32N6570_TEST_ROUNDS       (100U)
#define MTFS_STM32N6570_TEST_PROFILE_NAME "stress"
#else
#error MTFS_STM32N6570_TEST_PROFILE must be 1, 2, or 3
#endif

static mtfs_stm32_sdmmc_context_t sd_context;
static uint8_t sector_zero_single[MTFS_STM32_SDMMC_SECTOR_SIZE];
static uint8_t sector_zero_multi[MTFS_STM32_SDMMC_SECTOR_SIZE * 2U];

static void target_reporter(
    void *opaque, mtfs_test_event_t event, const char *test_name,
    const char *file, int line, const char *message,
    unsigned int checks, unsigned int failures)
{
    (void)opaque;
    if (event == MTFS_TEST_EVENT_BEGIN) {
        tm_printf((UB *)"[TEST] %s: BEGIN\n", (UB *)test_name);
    } else if (event == MTFS_TEST_EVENT_CHECK_FAILED) {
        tm_printf((UB *)"[TEST] %s: FAIL %s:%d: %s\n",
            (UB *)test_name, (UB *)file, line, (UB *)message);
    } else {
        tm_printf((UB *)"[TEST] %s: %s (checks=%u failures=%u)\n",
            (UB *)test_name, failures == 0U ? (UB *)"PASS" : (UB *)"FAIL",
            checks, failures);
    }
}

static int target_check_idma_diagnostics(
    mtfs_test_t *test, const mtfs_stm32_sdmmc_context_t *context)
{
    const mtfs_stm32_sdmmc_diagnostics_t *diagnostics =
        &context->diagnostics;

    if (!context->config.use_idma) {
        return 0;
    }
    (void)MTFS_TEST_CHECK(test, diagnostics->irq_entries > 0U,
        "SDMMC2 IRQ reached the T-Kernel high-level handler");
    (void)MTFS_TEST_CHECK(test, diagnostics->rx_complete_callbacks > 0U,
        "SD Rx complete callback reached");
    (void)MTFS_TEST_CHECK(test, diagnostics->tx_complete_callbacks > 0U,
        "SD Tx complete callback reached");
    (void)MTFS_TEST_CHECK(test, diagnostics->error_callbacks == 0U,
        "SD error callback count remains zero");
    (void)MTFS_TEST_CHECK(test, diagnostics->read_single_starts > 0U,
        "single-block IDMA read started");
    (void)MTFS_TEST_CHECK(test, diagnostics->read_multi_starts > 0U,
        "multi-block IDMA read started");
    (void)MTFS_TEST_CHECK(test, diagnostics->write_multi_starts > 0U,
        "multi-block IDMA write started through FatFs");
    (void)MTFS_TEST_CHECK(test, diagnostics->read_max_blocks >= 2U,
        "IDMA read maximum block count is at least two");
    (void)MTFS_TEST_CHECK(test, diagnostics->write_max_blocks >= 2U,
        "IDMA write maximum block count is at least two");
    return test->failures == 0U ? 0 : 1;
}

static void target_print_diagnostics(
    const mtfs_stm32_sdmmc_context_t *context)
{
    const mtfs_stm32_sdmmc_diagnostics_t *diagnostics =
        &context->diagnostics;
    tm_printf((UB *)"[mtfs] irq=%u rx=%u tx=%u err=%u abort=%u timeout=%u/%u\n",
        diagnostics->irq_entries,
        diagnostics->rx_complete_callbacks,
        diagnostics->tx_complete_callbacks,
        diagnostics->error_callbacks,
        diagnostics->aborts,
        diagnostics->completion_timeouts,
        diagnostics->card_state_timeouts);
    tm_printf((UB *)"[mtfs] read single=%u multi=%u max=%u; write single=%u multi=%u max=%u\n",
        diagnostics->read_single_starts,
        diagnostics->read_multi_starts,
        diagnostics->read_max_blocks,
        diagnostics->write_single_starts,
        diagnostics->write_multi_starts,
        diagnostics->write_max_blocks);
}

static void target_coordinator(INT start_code, void *opaque)
{
    mtfs_stm32_sdmmc_config_t config;
    mtfs_stm32n6570_dk_rif_diagnostics_t rif;
    unsigned int round;
    int overall_failure = 0;

    (void)start_code;
    (void)opaque;
    mtfs_stm32n6570_dk_sdmmc_config(&config);
    mtfs_stm32n6570_dk_get_rif_diagnostics(&rif);
    tm_printf((UB *)"\n[mtfs] STM32N6570-DK Phase 3: profile=%s rounds=%u path=%s\n",
        (UB *)MTFS_STM32N6570_TEST_PROFILE_NAME,
        MTFS_STM32N6570_TEST_ROUNDS,
        config.use_idma ? (UB *)"IDMA+IRQ" : (UB *)"polling fallback");
    tm_printf((UB *)"[mtfs] cache I=%s D=%s CCR=0x%08x\n",
        (SCB->CCR & SCB_CCR_IC_Msk) ? (UB *)"enabled" : (UB *)"disabled",
        (SCB->CCR & SCB_CCR_DC_Msk) ? (UB *)"enabled" : (UB *)"disabled",
        SCB->CCR);
    tm_printf((UB *)"[mtfs] RIF ready=%u master[3]=0x%08x sec=0x%08x priv=0x%08x\n",
        rif.ready, rif.master_attribute, rif.slave_secure,
        rif.slave_privileged);
    if (((SCB->CCR & (SCB_CCR_IC_Msk | SCB_CCR_DC_Msk)) !=
            (SCB_CCR_IC_Msk | SCB_CCR_DC_Msk)) ||
        (config.use_idma && !rif.ready)) {
        tm_printf((UB *)"[mtfs] platform precondition FAIL\n");
        overall_failure = 1;
    }

    for (round = 1U; round <= MTFS_STM32N6570_TEST_ROUNDS; ++round) {
        mtfs_block_device_t *device = NULL;
        mtfs_block_geometry_t geometry;
        mtfs_error_t error;
        mtfs_test_t test;
        int case_result;
        int registered = 0;
        int context_ready = 0;
        int round_failure = 0;

        tm_printf((UB *)"[mtfs] round %u/%u BEGIN\n",
            round, MTFS_STM32N6570_TEST_ROUNDS);
        error = mtfs_stm32_sdmmc_context_init(&sd_context, &config);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[mtfs] context init FAIL mtfs=%d tk=%d hal=%u/0x%08x\n",
                error, sd_context.last_kernel_error,
                sd_context.last_hal_status, sd_context.last_hal_error);
            round_failure = 1;
            goto round_done;
        }
        context_ready = 1;
        device = mtfs_stm32_sdmmc_block_device(&sd_context);
        tm_printf((UB *)"[mtfs] context=%p bounce=%p size=%u align32=%u\n",
            &sd_context, sd_context.bounce_buffer,
            (UW)sizeof(sd_context.bounce_buffer),
            ((uintptr_t)sd_context.bounce_buffer & 31U) == 0U);

        error = mtfs_block_initialize(device);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[mtfs] SD init FAIL mtfs=%d tk=%d hal=%u/0x%08x\n",
                error, sd_context.last_kernel_error,
                sd_context.last_hal_status, sd_context.last_hal_error);
            round_failure = 1;
            goto round_done;
        }
        error = mtfs_block_registry_register(0U, device);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[mtfs] pdrv 0 register FAIL: %d\n", error);
            round_failure = 1;
            goto round_done;
        }
        registered = 1;

        error = mtfs_block_get_geometry(device, &geometry);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[mtfs] geometry FAIL: %d\n", error);
            round_failure = 1;
            goto round_done;
        }
        tm_printf((UB *)"[mtfs] geometry sectors=%u size=%u erase=%u\n",
            (UW)geometry.sector_count, geometry.sector_size,
            geometry.erase_block_size);
        error = mtfs_block_read(device, sector_zero_single, 0U, 1U);
        if (error == MTFS_OK) {
            error = mtfs_block_read(device, sector_zero_multi, 0U, 2U);
        }
        if ((error != MTFS_OK) ||
            (memcmp(sector_zero_single, sector_zero_multi,
                sizeof(sector_zero_single)) != 0)) {
            tm_printf((UB *)"[mtfs] raw read/compare FAIL: %d\n", error);
            round_failure = 1;
            goto round_done;
        }
        tm_printf((UB *)"[mtfs] raw read PASS signature=%02x%02x (%s)\n",
            sector_zero_single[510], sector_zero_single[511],
            (sector_zero_single[510] == 0x55U &&
                sector_zero_single[511] == 0xAAU)
                ? (UB *)"present" : (UB *)"not required");

        mtfs_test_begin(&test, "fatfs_roundtrip", target_reporter, NULL);
        case_result = test_fatfs_roundtrip(&test, "0:");
        if ((mtfs_test_finish(&test) != 0) || (case_result != 0)) {
            round_failure = 1;
        }

        mtfs_test_begin(&test, "fatfs_concurrent_microtkernel",
            target_reporter, NULL);
        case_result = mtfs_target_run_concurrent(&test, "0:", round);
        if ((mtfs_test_finish(&test) != 0) || (case_result != 0)) {
            round_failure = 1;
        }

        mtfs_test_begin(&test, "sdmmc_idma_diagnostics",
            target_reporter, NULL);
        case_result = target_check_idma_diagnostics(&test, &sd_context);
        if ((mtfs_test_finish(&test) != 0) || (case_result != 0)) {
            round_failure = 1;
        }
        target_print_diagnostics(&sd_context);

round_done:
        if (registered && (mtfs_block_registry_unregister(0U) != MTFS_OK)) {
            tm_printf((UB *)"[mtfs] registry cleanup FAIL\n");
            round_failure = 1;
        }
        if (context_ready) {
            error = mtfs_stm32_sdmmc_context_deinit(&sd_context);
            if (error != MTFS_OK) {
                tm_printf((UB *)"[mtfs] context cleanup FAIL: %d\n", error);
                round_failure = 1;
            }
        }
        tm_printf((UB *)"[mtfs] round %u/%u %s\n", round,
            MTFS_STM32N6570_TEST_ROUNDS,
            round_failure ? (UB *)"FAIL" : (UB *)"PASS");
        if (round_failure) {
            overall_failure = 1;
        }
    }
    tm_printf((UB *)"[mtfs] PHASE 3 RUN %s\n",
        overall_failure ? (UB *)"FAIL" : (UB *)"PASS");
    tk_exd_tsk();
}

EXPORT INT usermain(void)
{
    T_CTSK coordinator = {
        .tskatr = TA_HLNG | TA_RNG3,
        .task = target_coordinator,
        .itskpri = 9,
        .stksz = 16U * 1024U
    };
    ID task_id = tk_cre_tsk(&coordinator);

    if (task_id <= 0) {
        tm_printf((UB *)"[mtfs] coordinator create FAIL: %d\n", task_id);
        goto park;
    }
    if (tk_sta_tsk(task_id, 0) < E_OK) {
        tm_printf((UB *)"[mtfs] coordinator start FAIL\n");
        (void)tk_del_tsk(task_id);
    }
park:
    for (;;) {
        (void)tk_slp_tsk(TMO_FEVR);
    }
}
