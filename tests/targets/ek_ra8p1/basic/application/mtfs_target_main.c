#include <stddef.h>
#include <string.h>

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "ff.h"
#include "mtfs_block_device.h"
#include "mtfs_block_registry.h"
#include "core/mtfs_media.h"
#include "mtfs_media_service.h"
#include "mtfs_ra_sd_spi.h"
#include "mtfs_test.h"
#include "test_fatfs_roundtrip.h"
#include "mtfs_ra8p1_platform.h"
#include "mtfs_ra8p1_vector_cache.h"
#include "mtfs_target_concurrent.h"

#define MTFS_TEST_PROFILE_SMOKE  (1)
#define MTFS_TEST_PROFILE_NORMAL (2)
#define MTFS_TEST_PROFILE_STRESS (3)

#ifndef MTFS_RA8P1_TEST_PROFILE
#define MTFS_RA8P1_TEST_PROFILE MTFS_TEST_PROFILE_NORMAL
#endif

#ifndef MTFS_RA8P1_HOTPLUG_TEST
#define MTFS_RA8P1_HOTPLUG_TEST (0)
#endif

#define MTFS_TARGET_MEDIA_INSERTED  (UINT32_C(1) << 0)
#define MTFS_TARGET_MEDIA_REMOVED   (UINT32_C(1) << 1)
#define MTFS_TARGET_MEDIA_ERROR     (UINT32_C(1) << 2)
#define MTFS_TARGET_HOTPLUG_WAIT_MS (120000U)

#if MTFS_RA8P1_TEST_PROFILE == MTFS_TEST_PROFILE_SMOKE
#define MTFS_RA8P1_TEST_ROUNDS       (1U)
#define MTFS_RA8P1_TEST_PROFILE_NAME "smoke"
#elif MTFS_RA8P1_TEST_PROFILE == MTFS_TEST_PROFILE_NORMAL
#define MTFS_RA8P1_TEST_ROUNDS       (10U)
#define MTFS_RA8P1_TEST_PROFILE_NAME "normal"
#elif MTFS_RA8P1_TEST_PROFILE == MTFS_TEST_PROFILE_STRESS
#define MTFS_RA8P1_TEST_ROUNDS       (100U)
#define MTFS_RA8P1_TEST_PROFILE_NAME "stress"
#else
#error MTFS_RA8P1_TEST_PROFILE must be 1, 2, or 3
#endif

static mtfs_ra_sd_spi_context_t sd_context;
static mtfs_media_context_t media_context;
static mtfs_media_service_context_t media_service;
static volatile uint32_t media_inserted_events;
static volatile uint32_t media_removed_events;
static volatile uint32_t media_error_events;
static volatile uint32_t media_reinitialize_count;
static ID media_application_event_flag_id;
static uint8_t sector_zero_single[MTFS_RA_SD_SPI_SECTOR_SIZE];
static uint8_t sector_zero_multi[MTFS_RA_SD_SPI_SECTOR_SIZE * 2U];

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
    mtfs_ra8p1_card_detect_diagnostics_t cd;
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

    mtfs_ra8p1_get_card_detect_diagnostics(&cd);
    tm_printf((UB *)"[mtfs] CD raw=%u active=%s irq=%u rise=%u fall=%u debounce=%u/%u events=%u/%u/%u\n",
        cd.raw_level, cd.active_low ? (UB *)"low" : (UB *)"high",
        cd.irq_entries, cd.rising_edges, cd.falling_edges,
        media_context.diagnostics.debounce_starts,
        media_context.diagnostics.debounce_rechecks,
        media_inserted_events, media_removed_events, media_error_events);
}

#if MTFS_RA8P1_HOTPLUG_TEST
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

static int target_check_spi_diagnostics(
    mtfs_test_t *test, const mtfs_ra_sd_spi_context_t *context)
{
    const mtfs_ra_sd_spi_diagnostics_t *diagnostics = &context->diagnostics;

    (void)MTFS_TEST_CHECK(test, diagnostics->transfer_starts > 0U,
        "SCI_B SPI transfers started");
    (void)MTFS_TEST_CHECK(test, diagnostics->transfer_completions > 0U,
        "SCI_B SPI completion callbacks reached");
    (void)MTFS_TEST_CHECK(test, diagnostics->transfer_errors == 0U,
        "SCI_B SPI callback error count remains zero");
    (void)MTFS_TEST_CHECK(test, diagnostics->read_sectors > 0U,
        "SD SPI sector reads completed");
    (void)MTFS_TEST_CHECK(test, diagnostics->write_sectors > 0U,
        "SD SPI sector writes completed");
    return test->failures == 0U ? 0 : 1;
}

static void target_print_diagnostics(
    const mtfs_ra_sd_spi_context_t *context)
{
    const mtfs_ra_sd_spi_diagnostics_t *diagnostics = &context->diagnostics;

    tm_printf((UB *)"[mtfs] spi starts=%u complete=%u errors=%u read=%u write=%u\n",
        diagnostics->transfer_starts,
        diagnostics->transfer_completions,
        diagnostics->transfer_errors,
        diagnostics->read_sectors,
        diagnostics->write_sectors);
    tm_printf((UB *)"[mtfs] media removal hints=%u wait wakeups=%u\n",
        diagnostics->media_removal_notifications,
        diagnostics->media_wait_wakeups);
    tm_printf((UB *)"[mtfs] last mtfs=%d tk=%d fsp=%d r1=0x%02x bitrate=%u\n",
        context->last_error, context->last_kernel_error,
        context->last_fsp_error, context->last_r1,
        context->current_bitrate_hz);
}

static void target_coordinator(INT start_code, void *opaque)
{
    mtfs_ra_sd_spi_config_t config;
    unsigned int round;
    int overall_failure = 0;
#if MTFS_RA8P1_HOTPLUG_TEST
    T_CFLG media_flag_config = {
        .flgatr = TA_TFIFO,
        .iflgptn = 0U
    };
#endif

    (void)start_code;
    (void)opaque;
    mtfs_ra8p1_sd_spi_config(&config);
#if MTFS_RA8P1_HOTPLUG_TEST
    media_application_event_flag_id = tk_cre_flg(&media_flag_config);
    if (media_application_event_flag_id <= 0) {
        tm_printf((UB *)"[mtfs] application media event flag create FAIL: %d\n",
            media_application_event_flag_id);
        tk_exd_tsk();
    }
#endif

    tm_printf((UB *)"\n[mtfs] EK-RA8P1 Phase 3.2: profile=%s rounds=%u path=SCI_B SPI+IRQ CD hotplug=%s\n",
        (UB *)MTFS_RA8P1_TEST_PROFILE_NAME, MTFS_RA8P1_TEST_ROUNDS,
        MTFS_RA8P1_HOTPLUG_TEST ? (UB *)"on" : (UB *)"off");
    tm_printf((UB *)"[mtfs] cache: I=%s D=%s fallback=%s VTOR=0x%08x\n",
        (g_mtfs_ra8p1_vector_cache_diagnostics.ccr_at_hal_entry &
            SCB_CCR_IC_Msk) ? (UB *)"enabled" : (UB *)"disabled",
        (g_mtfs_ra8p1_vector_cache_diagnostics.ccr_at_hal_entry &
            SCB_CCR_DC_Msk) ? (UB *)"enabled" : (UB *)"disabled",
        g_mtfs_ra8p1_vector_cache_diagnostics.fallback_active
            ? (UB *)"ACTIVE" : (UB *)"off",
        g_mtfs_ra8p1_vector_cache_diagnostics.vtor_after_relocation);
    tm_printf((UB *)"[mtfs] vector: [0x%08x,0x%08x) size=%u line=%u cleans=%u\n",
        g_mtfs_ra8p1_vector_cache_diagnostics.vector_start,
        g_mtfs_ra8p1_vector_cache_diagnostics.vector_start +
            g_mtfs_ra8p1_vector_cache_diagnostics.vector_size,
        g_mtfs_ra8p1_vector_cache_diagnostics.vector_size,
        g_mtfs_ra8p1_vector_cache_diagnostics.dcache_line_size,
        g_mtfs_ra8p1_vector_cache_diagnostics.clean_count);
    if (g_mtfs_ra8p1_vector_cache_diagnostics.fallback_active ||
        ((SCB->CCR & (SCB_CCR_IC_Msk | SCB_CCR_DC_Msk)) !=
            (SCB_CCR_IC_Msk | SCB_CCR_DC_Msk))) {
        tm_printf((UB *)"[mtfs] cache precondition FAIL: I/D cache required\n");
        overall_failure = 1;
    }

    for (round = 1U; round <= MTFS_RA8P1_TEST_ROUNDS; ++round) {
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
            round, MTFS_RA8P1_TEST_ROUNDS);
        error = mtfs_ra_sd_spi_context_init(&sd_context, &config);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[mtfs] context init FAIL mtfs=%d tk=%d fsp=%d\n",
                error, sd_context.last_kernel_error,
                sd_context.last_fsp_error);
            round_failure = 1;
            goto round_done;
        }
        context_ready = 1;
        error = mtfs_ra8p1_card_detect_start(&media_context,
            &media_service, &sd_context, target_media_event, NULL);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[mtfs] card detect start FAIL mtfs=%d tk=%d\n",
                error, media_service.last_kernel_error);
            round_failure = 1;
            goto round_done;
        }
        media_ready = 1;
        device = mtfs_ra_sd_spi_block_device(&sd_context);
        tm_printf((UB *)"[mtfs] context=%p dummy=%p size=%u cd-debounce=%u ms\n",
            &sd_context, sd_context.dummy_tx,
            (UW)sizeof(sd_context.dummy_tx),
            media_context.config.debounce_ms);
        {
            mtfs_ra8p1_card_detect_diagnostics_t cd;
            mtfs_ra8p1_get_card_detect_diagnostics(&cd);
            tm_printf((UB *)"[mtfs] CD initial raw=%u configured-active=%s irq=%u (verify level on hardware)\n",
                cd.raw_level, cd.active_low ? (UB *)"low" : (UB *)"high",
                cd.irq_entries);
        }

#if MTFS_RA8P1_HOTPLUG_TEST
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
            tm_printf((UB *)"[mtfs] SD init FAIL mtfs=%d tk=%d fsp=%d r1=0x%02x\n",
                error, sd_context.last_kernel_error,
                sd_context.last_fsp_error, sd_context.last_r1);
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
        tm_printf((UB *)"[mtfs] geometry sectors=%u size=%u erase=%u type=%s\n",
            (UW)geometry.sector_count, geometry.sector_size,
            geometry.erase_block_size,
            sd_context.card_type == MTFS_RA_SD_CARD_SDHC_SDXC
                ? (UB *)"SDHC/SDXC" : (UB *)"SDSC");
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

        mtfs_test_begin(&test, "sd_spi_diagnostics",
            target_reporter, NULL);
        case_result = target_check_spi_diagnostics(&test, &sd_context);
        if ((mtfs_test_finish(&test) != 0) || (case_result != 0)) {
            round_failure = 1;
        }
        target_print_diagnostics(&sd_context);

#if MTFS_RA8P1_HOTPLUG_TEST
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
        if (!target_wait_media_event(MTFS_TARGET_MEDIA_INSERTED,
                "re-INSERTED")) {
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
            mtfs_ra8p1_card_detect_diagnostics_t cd;
            mtfs_ra8p1_get_card_detect_diagnostics(&cd);
            tm_printf((UB *)"[mtfs] CD raw=%u active=%s irq=%u rise=%u fall=%u inserted=%u removed=%u error=%u state=%u reinit=%u\n",
                cd.raw_level, cd.active_low ? (UB *)"low" : (UB *)"high",
                cd.irq_entries, cd.rising_edges, cd.falling_edges,
                media_inserted_events, media_removed_events,
                media_error_events, (UW)mtfs_media_state(&media_context),
                media_reinitialize_count);
            if (mtfs_ra8p1_card_detect_stop() != MTFS_OK) {
                tm_printf((UB *)"[mtfs] card detect cleanup FAIL tk=%d task=%d flag=%d\n",
                    media_service.last_kernel_error,
                    media_service.task_id, media_service.event_flag_id);
                round_failure = 1;
            }
        }
        if (registered && (mtfs_block_registry_unregister(0U) != MTFS_OK)) {
            tm_printf((UB *)"[mtfs] registry cleanup FAIL\n");
            round_failure = 1;
        }
        if (context_ready) {
            error = mtfs_ra_sd_spi_context_deinit(&sd_context);
            if (error != MTFS_OK) {
                tm_printf((UB *)"[mtfs] context cleanup FAIL: %d\n", error);
                round_failure = 1;
            }
        }
        tm_printf((UB *)"[mtfs] round %u/%u %s\n", round,
            MTFS_RA8P1_TEST_ROUNDS,
            round_failure ? (UB *)"FAIL" : (UB *)"PASS");
        if (round_failure) {
            overall_failure = 1;
        }
    }

    tm_printf((UB *)"[mtfs] PHASE 3.2 RUN %s\n",
        overall_failure ? (UB *)"FAIL" : (UB *)"PASS");
#if MTFS_RA8P1_HOTPLUG_TEST
    if (media_application_event_flag_id > 0) {
        (void)tk_del_flg(media_application_event_flag_id);
        media_application_event_flag_id = 0;
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
