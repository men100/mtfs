#include <stddef.h>
#include <string.h>

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "stm32n6xx.h"

#include "mtfs_block_device.h"
#include "mtfs_block_registry.h"
#include "ff.h"
#include "mtfs_stm32_sdmmc.h"
#include "mtfs_test.h"
#include "test_fatfs_roundtrip.h"
#include "mtfs_stm32n6570_dk_platform.h"
#include "mtfs_target_concurrent.h"
#ifndef MTFS_FF_FS_NORTC
#define MTFS_FF_FS_NORTC (1)
#endif
#if !MTFS_FF_FS_NORTC
#include "mtfs_stm32_rtc.h"
#endif
#ifndef MTFS_TARGET_RTC_CONSOLE
#define MTFS_TARGET_RTC_CONSOLE (0)
#endif
#if !MTFS_FF_FS_NORTC && MTFS_TARGET_RTC_CONSOLE
#include "mtfs_rtc_set_app.h"
#include "test_fatfs_timestamp.h"
#define MTFS_TARGET_RTC_CONSOLE_ACTIVE (1)
#else
#define MTFS_TARGET_RTC_CONSOLE_ACTIVE (0)
#endif

#define MTFS_TEST_PROFILE_SMOKE   (1)
#define MTFS_TEST_PROFILE_NORMAL  (2)
#define MTFS_TEST_PROFILE_STRESS  (3)

#ifndef MTFS_STM32N6570_TEST_PROFILE
#define MTFS_STM32N6570_TEST_PROFILE MTFS_TEST_PROFILE_NORMAL
#endif

#ifndef MTFS_STM32N6570_HOTPLUG_TEST
#define MTFS_STM32N6570_HOTPLUG_TEST (0)
#endif

#define MTFS_TARGET_MEDIA_INSERTED (UINT32_C(1) << 0)
#define MTFS_TARGET_MEDIA_REMOVED  (UINT32_C(1) << 1)
#define MTFS_TARGET_MEDIA_ERROR    (UINT32_C(1) << 2)
#define MTFS_TARGET_HOTPLUG_WAIT_MS (120000)

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
static mtfs_media_context_t media_context;
static mtfs_media_service_context_t media_service;
static volatile uint32_t media_inserted_events;
static volatile uint32_t media_removed_events;
static volatile uint32_t media_error_events;
static volatile uint32_t media_reinitialize_count;
static ID media_application_event_flag_id;
static uint8_t sector_zero_single[MTFS_STM32_SDMMC_SECTOR_SIZE];
static uint8_t sector_zero_multi[MTFS_STM32_SDMMC_SECTOR_SIZE * 2U];
#if !MTFS_FF_FS_NORTC
static mtfs_stm32_rtc_context_t rtc_context;
static ID rtc_mutex_id;

static mtfs_error_t target_rtc_lock(void *opaque)
{
    ID mutex_id = *(ID *)opaque;
    return tk_loc_mtx(mutex_id, TMO_FEVR) == E_OK ? MTFS_OK : MTFS_ERROR_IO;
}

static void target_rtc_unlock(void *opaque)
{
    ID mutex_id = *(ID *)opaque;
    (void)tk_unl_mtx(mutex_id);
}
#endif

#if MTFS_TARGET_RTC_CONSOLE_ACTIVE
static void target_rtc_write(void *opaque, const char *text)
{
    (void)opaque;
    (void)tm_putstring((const UB *)text);
}

static int target_rtc_command(void *opaque, const char *line);

static void target_rtc_console(void)
{
    mtfs_rtc_set_app_t app;
    mtfs_rtc_set_app_init(&app, target_rtc_write, NULL);
    mtfs_rtc_set_app_set_extension(&app, target_rtc_command, NULL,
        "test-fatfs-time           verify FatFs timestamp against RTC\r\n");
    mtfs_rtc_set_app_banner(&app);
    for (;;) {
        mtfs_rtc_set_app_feed(&app, (char)tm_getchar(1));
    }
}
#endif

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

static void target_media_event(void *opaque, mtfs_media_event_t event,
    mtfs_media_state_t state)
{
    mtfs_stm32n6570_dk_card_detect_diagnostics_t cd;
    (void)opaque;
    if (event == MTFS_MEDIA_EVENT_INSERTED) {
        ++media_inserted_events;
        tm_printf((UB *)"[mtfs] media INSERTED state=%u; application must initialize/register/mount\n",
            (UW)state);
        if (media_application_event_flag_id > 0) {
            (void)tk_set_flg(media_application_event_flag_id,
                MTFS_TARGET_MEDIA_INSERTED);
        }
    } else if (event == MTFS_MEDIA_EVENT_REMOVED) {
        ++media_removed_events;
        tm_printf((UB *)"[mtfs] media REMOVED state=%u; application must stop requests/unmount/unregister\n",
            (UW)state);
        if (media_application_event_flag_id > 0) {
            (void)tk_set_flg(media_application_event_flag_id,
                MTFS_TARGET_MEDIA_REMOVED);
        }
    } else {
        ++media_error_events;
        tm_printf((UB *)"[mtfs] media ERROR state=%u\n", (UW)state);
        if (media_application_event_flag_id > 0) {
            (void)tk_set_flg(media_application_event_flag_id,
                MTFS_TARGET_MEDIA_ERROR);
        }
    }
    mtfs_stm32n6570_dk_get_card_detect_diagnostics(&cd);
    tm_printf((UB *)"[mtfs] CD raw=%u active=%s irq=%u rise=%u fall=%u debounce=%u/%u events=%u/%u/%u\n",
        cd.raw_level, cd.active_low ? (UB *)"low" : (UB *)"high",
        cd.irq_entries, cd.rising_edges, cd.falling_edges,
        media_context.diagnostics.debounce_starts,
        media_context.diagnostics.debounce_rechecks,
        media_inserted_events, media_removed_events, media_error_events);
}

#if MTFS_STM32N6570_HOTPLUG_TEST
static int target_wait_media_event(UINT expected, const char *operation)
{
    UINT events = 0U;
    ER result = tk_wai_flg(media_application_event_flag_id,
        expected | MTFS_TARGET_MEDIA_ERROR, TWF_ORW | TWF_BITCLR,
        &events, MTFS_TARGET_HOTPLUG_WAIT_MS);
    if ((result < E_OK) || ((events & expected) == 0U)) {
        tm_printf((UB *)"[mtfs] %s wait FAIL tk=%d events=0x%08x\n",
            (UB *)operation, result, events);
        return 0;
    }
    return 1;
}
#endif

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
    tm_printf((UB *)"[mtfs] media removal hints=%u wait wakeups=%u\n",
        diagnostics->media_removal_notifications,
        diagnostics->media_wait_wakeups);
    tm_printf((UB *)"[mtfs] read single=%u multi=%u max=%u; write single=%u multi=%u max=%u\n",
        diagnostics->read_single_starts,
        diagnostics->read_multi_starts,
        diagnostics->read_max_blocks,
        diagnostics->write_single_starts,
        diagnostics->write_multi_starts,
        diagnostics->write_max_blocks);
    tm_printf((UB *)"[mtfs] last mtfs=%d tk=%d hal=%u/0x%08x clkcr(snapshot)=0x%08x hwfc=%u div=%u\n",
        context->last_error,
        context->last_kernel_error,
        context->last_hal_status,
        context->last_hal_error,
        diagnostics->last_clkcr,
        (diagnostics->last_clkcr & SDMMC_CLKCR_HWFC_EN) != 0U,
        diagnostics->last_clkcr & SDMMC_CLKCR_CLKDIV);
}

#if MTFS_TARGET_RTC_CONSOLE_ACTIVE
static void target_print_timestamp_datetime(
    const char *label, const mtfs_datetime_t *datetime)
{
    tm_printf((UB *)"%s%04u-%02u-%02u %02u:%02u:%02u\n",
        (UB *)label, datetime->year, datetime->month, datetime->day,
        datetime->hour, datetime->minute, datetime->second);
}

static int target_run_fatfs_time_test(void)
{
    mtfs_stm32_sdmmc_config_t config;
    mtfs_block_device_t *device = NULL;
    mtfs_fatfs_timestamp_result_t timestamp_result;
    mtfs_time_status_t rtc_status = MTFS_TIME_STATUS_UNAVAILABLE;
    mtfs_error_t error;
    mtfs_test_t test;
    int case_result = 1;
    int test_failure = 1;
    int registered = 0;
    int context_ready = 0;
    int media_ready = 0;

    if ((mtfs_time_get_status(&rtc_status) != MTFS_OK) ||
        (rtc_status != MTFS_TIME_STATUS_VALID)) {
        tm_printf((UB *)"[mtfs] FAT timestamp command FAIL: RTC state=%u\n",
            (UW)rtc_status);
        return 1;
    }
    mtfs_stm32n6570_dk_sdmmc_config(&config);
    error = mtfs_stm32_sdmmc_context_init(&sd_context, &config);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] timestamp context init FAIL: %d\n", error);
        goto cleanup;
    }
    context_ready = 1;
    error = mtfs_stm32n6570_dk_card_detect_start(&media_context,
        &media_service, &sd_context, target_media_event, NULL);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] timestamp card detect start FAIL: %d\n",
            error);
        goto cleanup;
    }
    media_ready = 1;
    device = mtfs_stm32_sdmmc_block_device(&sd_context);
    error = mtfs_block_initialize(device);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] timestamp SD init FAIL: %d\n", error);
        goto cleanup;
    }
    error = mtfs_block_registry_register(0U, device);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] timestamp pdrv 0 register FAIL: %d\n",
            error);
        goto cleanup;
    }
    registered = 1;

    mtfs_test_begin(&test, "fatfs_timestamp", target_reporter, NULL);
    case_result = test_fatfs_timestamp(&test, "0:", &timestamp_result);
    test_failure = (mtfs_test_finish(&test) != 0) || (case_result != 0);
    if (timestamp_result.fat_file != 0U) {
        target_print_timestamp_datetime(
            "RTC before : ", &timestamp_result.rtc_before);
        target_print_timestamp_datetime(
            "File time  : ", &timestamp_result.file_time);
        target_print_timestamp_datetime(
            "RTC after  : ", &timestamp_result.rtc_after);
        tm_printf((UB *)"FAT raw    : 0x%08x\n",
            timestamp_result.fat_file);
    }

cleanup:
    if (media_ready &&
        (mtfs_stm32n6570_dk_card_detect_stop() != MTFS_OK)) {
        tm_printf((UB *)"[mtfs] timestamp card detect cleanup FAIL\n");
        test_failure = 1;
    }
    if (registered && (mtfs_block_registry_unregister(0U) != MTFS_OK)) {
        tm_printf((UB *)"[mtfs] timestamp registry cleanup FAIL\n");
        test_failure = 1;
    }
    if (context_ready &&
        (mtfs_stm32_sdmmc_context_deinit(&sd_context) != MTFS_OK)) {
        tm_printf((UB *)"[mtfs] timestamp context cleanup FAIL\n");
        test_failure = 1;
    }
    if (context_ready && test_failure) {
        target_print_diagnostics(&sd_context);
    }
    tm_printf((UB *)"[mtfs] FAT timestamp command %s\n",
        test_failure ? (UB *)"FAIL" : (UB *)"PASS");
    return test_failure ? 1 : 0;
}

static int target_rtc_command(void *opaque, const char *line)
{
    (void)opaque;
    if (strcmp(line, "test-fatfs-time") != 0) {
        return 0;
    }
    (void)target_run_fatfs_time_test();
    return 1;
}
#endif

static void target_coordinator(INT start_code, void *opaque)
{
    mtfs_stm32_sdmmc_config_t config;
    mtfs_stm32n6570_dk_rif_diagnostics_t rif;
    unsigned int round;
    int overall_failure = 0;
#if !MTFS_FF_FS_NORTC
    mtfs_error_t rtc_error;
    T_CMTX rtc_mutex = {
        .mtxatr = TA_INHERIT
    };
#endif
#if MTFS_STM32N6570_HOTPLUG_TEST
    T_CFLG media_flag_config = {
        .flgatr = TA_TFIFO,
        .iflgptn = 0U
    };
#endif

    (void)start_code;
    (void)opaque;
#if !MTFS_FF_FS_NORTC
    rtc_mutex_id = tk_cre_mtx(&rtc_mutex);
    if (rtc_mutex_id <= 0) {
        tm_printf((UB *)"[mtfs] RTC mutex create FAIL: %d\n", rtc_mutex_id);
        tk_exd_tsk();
    }
    rtc_error = mtfs_stm32_rtc_init(
        &rtc_context, target_rtc_lock, target_rtc_unlock, &rtc_mutex_id);
    if (rtc_error == MTFS_OK) {
        rtc_error = mtfs_time_provider_register(
            mtfs_stm32_rtc_provider(&rtc_context));
    }
    if (rtc_error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] RTC provider init FAIL: %d reset=0x%08x\n",
            rtc_error, rtc_context.reset_flags_at_init);
        overall_failure = 1;
    } else {
        mtfs_time_status_t rtc_status;
        (void)mtfs_time_get_status(&rtc_status);
        tm_printf((UB *)"[mtfs] RTC provider state=%u source=LSI local-time reset=0x%08x\n",
            (UW)rtc_status, rtc_context.reset_flags_at_init);
    }
#endif
    mtfs_stm32n6570_dk_sdmmc_config(&config);
#if MTFS_STM32N6570_HOTPLUG_TEST
    media_application_event_flag_id = tk_cre_flg(&media_flag_config);
    if (media_application_event_flag_id <= 0) {
        tm_printf((UB *)"[mtfs] application media event flag create FAIL: %d\n",
            media_application_event_flag_id);
        tk_exd_tsk();
    }
#endif
    mtfs_stm32n6570_dk_get_rif_diagnostics(&rif);
    tm_printf((UB *)"\n[mtfs] STM32N6570-DK Phase 3: profile=%s rounds=%u path=%s hotplug=%s\n",
        (UB *)MTFS_STM32N6570_TEST_PROFILE_NAME,
        MTFS_STM32N6570_TEST_ROUNDS,
        config.use_idma ? (UB *)"IDMA+IRQ" : (UB *)"polling fallback",
        MTFS_STM32N6570_HOTPLUG_TEST ? (UB *)"on" : (UB *)"off");
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
        int media_ready = 0;
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
        error = mtfs_stm32n6570_dk_card_detect_start(&media_context,
            &media_service, &sd_context, target_media_event, NULL);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[mtfs] card detect start FAIL mtfs=%d tk=%d\n",
                error, media_service.last_kernel_error);
            round_failure = 1;
            goto round_done;
        }
        media_ready = 1;
        device = mtfs_stm32_sdmmc_block_device(&sd_context);
        tm_printf((UB *)"[mtfs] context=%p bounce=%p size=%u align32=%u cd-debounce=%u ms\n",
            &sd_context, sd_context.bounce_buffer,
            (UW)sizeof(sd_context.bounce_buffer),
            ((uintptr_t)sd_context.bounce_buffer & 31U) == 0U,
            media_context.config.debounce_ms);

#if MTFS_STM32N6570_HOTPLUG_TEST
        if (!mtfs_media_is_present(&media_context)) {
            mtfs_block_status_t absent_status = 0U;
            mtfs_error_t absent_error =
                mtfs_block_status(device, &absent_status);
            if ((absent_error != MTFS_ERROR_NO_MEDIA) ||
                ((absent_status & MTFS_BLOCK_STATUS_MEDIA_PRESENT) != 0U)) {
                tm_printf((UB *)"[mtfs] initial ABSENT status FAIL: %d/0x%08x\n",
                    absent_error, absent_status);
                round_failure = 1;
                goto round_done;
            }
            tm_printf((UB *)"[mtfs] initial ABSENT status PASS: mtfs=%d flags=0x%08x\n",
                absent_error, absent_status);
            tm_printf((UB *)"[mtfs] ACTION REQUIRED: INSERT card now; waiting up to %u ms\n",
                MTFS_TARGET_HOTPLUG_WAIT_MS);
            if (!target_wait_media_event(MTFS_TARGET_MEDIA_INSERTED,
                    "initial INSERTED")) {
                round_failure = 1;
                goto round_done;
            }
        }
#endif

        error = mtfs_block_initialize(device);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[mtfs] SD init FAIL mtfs=%d tk=%d hal=%u/0x%08x\n",
                error, sd_context.last_kernel_error,
                sd_context.last_hal_status, sd_context.last_hal_error);
            target_print_diagnostics(&sd_context);
            round_failure = 1;
            goto round_done;
        }
        ++media_reinitialize_count;
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
            target_print_diagnostics(&sd_context);
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
            target_print_diagnostics(&sd_context);
            round_failure = 1;
        }

        mtfs_test_begin(&test, "fatfs_concurrent_microtkernel",
            target_reporter, NULL);
        case_result = mtfs_target_run_concurrent(&test, "0:", round);
        if ((mtfs_test_finish(&test) != 0) || (case_result != 0)) {
            target_print_diagnostics(&sd_context);
            round_failure = 1;
        }

        mtfs_test_begin(&test, "sdmmc_idma_diagnostics",
            target_reporter, NULL);
        case_result = target_check_idma_diagnostics(&test, &sd_context);
        if ((mtfs_test_finish(&test) != 0) || (case_result != 0)) {
            round_failure = 1;
        }
        target_print_diagnostics(&sd_context);

#if MTFS_STM32N6570_HOTPLUG_TEST
        tm_printf((UB *)"[mtfs] ACTION REQUIRED: REMOVE card now; I/O is idle and files are closed/synced; waiting up to %u ms\n",
            MTFS_TARGET_HOTPLUG_WAIT_MS);
        if (!target_wait_media_event(MTFS_TARGET_MEDIA_REMOVED, "REMOVED")) {
            round_failure = 1;
            goto round_done;
        }
        (void)f_mount(NULL, "0:", 0U);
        {
            mtfs_block_status_t status = 0U;
            mtfs_error_t status_error = mtfs_block_status(device, &status);
            mtfs_error_t read_error = mtfs_block_read(
                device, sector_zero_single, 0U, 1U);
            if ((status_error != MTFS_ERROR_NO_MEDIA) ||
                ((status & MTFS_BLOCK_STATUS_MEDIA_PRESENT) != 0U) ||
                (read_error != MTFS_ERROR_NO_MEDIA)) {
                tm_printf((UB *)"[mtfs] removal contract FAIL status=%d/0x%08x read=%d\n",
                    status_error, status, read_error);
                round_failure = 1;
                goto round_done;
            }
        }
        if (mtfs_block_registry_unregister(0U) != MTFS_OK) {
            tm_printf((UB *)"[mtfs] removal unregister FAIL\n");
            round_failure = 1;
            goto round_done;
        }
        registered = 0;
        tm_printf((UB *)"[mtfs] HOTPLUG: removal contract PASS\n");
        tm_printf((UB *)"[mtfs] ACTION REQUIRED: REINSERT card now; waiting up to %u ms\n",
            MTFS_TARGET_HOTPLUG_WAIT_MS);
        if (!target_wait_media_event(MTFS_TARGET_MEDIA_INSERTED, "re-INSERTED")) {
            round_failure = 1;
            goto round_done;
        }
        error = mtfs_block_initialize(device);
        if ((error != MTFS_OK) ||
            (mtfs_block_registry_register(0U, device) != MTFS_OK)) {
            tm_printf((UB *)"[mtfs] reinitialize/register FAIL: %d\n", error);
            round_failure = 1;
            goto round_done;
        }
        ++media_reinitialize_count;
        registered = 1;
        mtfs_test_begin(&test, "fatfs_roundtrip_after_reinsert",
            target_reporter, NULL);
        case_result = test_fatfs_roundtrip(&test, "0:");
        if ((mtfs_test_finish(&test) != 0) || (case_result != 0)) {
            round_failure = 1;
        }
#endif

round_done:
        if (media_ready) {
            mtfs_stm32n6570_dk_card_detect_diagnostics_t cd;
            mtfs_stm32n6570_dk_get_card_detect_diagnostics(&cd);
            tm_printf((UB *)"[mtfs] CD raw=%u active=%s irq=%u rise=%u fall=%u inserted=%u removed=%u error=%u state=%u reinit=%u\n",
                cd.raw_level, cd.active_low ? (UB *)"low" : (UB *)"high",
                cd.irq_entries, cd.rising_edges, cd.falling_edges,
                media_inserted_events, media_removed_events,
                media_error_events, (UW)mtfs_media_state(&media_context),
                media_reinitialize_count);
            if (mtfs_stm32n6570_dk_card_detect_stop() != MTFS_OK) {
                tm_printf((UB *)"[mtfs] card detect cleanup FAIL tk=%d task=%d flag=%d\n",
                    media_service.last_kernel_error,
                    media_service.task_id, media_service.event_flag_id);
                round_failure = 1;
            }
            media_ready = 0;
        }
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
#if MTFS_STM32N6570_HOTPLUG_TEST
    if (media_application_event_flag_id > 0) {
        (void)tk_del_flg(media_application_event_flag_id);
        media_application_event_flag_id = 0;
    }
#endif
#if MTFS_TARGET_RTC_CONSOLE_ACTIVE
    if (rtc_error == MTFS_OK) {
        tm_printf((UB *)"[mtfs] RTC console ready after test run\n");
        target_rtc_console();
    }
#endif
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
