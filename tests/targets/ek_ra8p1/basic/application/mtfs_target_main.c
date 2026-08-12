#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "hal_data.h"
#include "bsp_pin_cfg.h"

#include "mtfs_block_device.h"
#include "mtfs_block_registry.h"
#include "mtfs_ra_sd_spi.h"
#include "mtfs_test.h"
#include "test_fatfs_roundtrip.h"
#include "mtfs_target_concurrent.h"
#include "mtfs_ra8p1_vector_cache.h"

#define MTFS_TEST_PROFILE_SMOKE  (1)
#define MTFS_TEST_PROFILE_NORMAL (2)
#define MTFS_TEST_PROFILE_STRESS (3)

#ifndef MTFS_RA8P1_TEST_PROFILE
#define MTFS_RA8P1_TEST_PROFILE MTFS_TEST_PROFILE_NORMAL
#endif

#if MTFS_RA8P1_TEST_PROFILE == MTFS_TEST_PROFILE_SMOKE
#define MTFS_RA8P1_TEST_ROUNDS (1U)
#define MTFS_RA8P1_TEST_PROFILE_NAME "smoke"
#elif MTFS_RA8P1_TEST_PROFILE == MTFS_TEST_PROFILE_NORMAL
#define MTFS_RA8P1_TEST_ROUNDS (10U)
#define MTFS_RA8P1_TEST_PROFILE_NAME "normal"
#elif MTFS_RA8P1_TEST_PROFILE == MTFS_TEST_PROFILE_STRESS
#define MTFS_RA8P1_TEST_ROUNDS (100U)
#define MTFS_RA8P1_TEST_PROFILE_NAME "stress"
#else
#error MTFS_RA8P1_TEST_PROFILE must be 1 (smoke), 2 (normal), or 3 (stress)
#endif

static mtfs_ra_sd_spi_context_t sd_context;
static uint8_t sector_zero[MTFS_RA_SD_SPI_SECTOR_SIZE];

static void target_reporter(
    void *opaque,
    mtfs_test_event_t event,
    const char *test_name,
    const char *file,
    int line,
    const char *message,
    unsigned int checks,
    unsigned int failures)
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

static void target_coordinator(INT start_code, void *opaque)
{
    mtfs_ra_sd_spi_config_t config;
    unsigned int round;
    int overall_failure = 0;

    (void)start_code;
    (void)opaque;
    tm_printf((UB *)"\n[mtfs] EK-RA8P1 Phase 2.1 start: profile=%s rounds=%u\n",
        (UB *)MTFS_RA8P1_TEST_PROFILE_NAME, MTFS_RA8P1_TEST_ROUNDS);
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

    config.device_name = "hspia";
    config.spi = &g_sci_spi0;
    config.ioport = &g_ioport;
    config.chip_select_pin = PMOD2_CTS;
    config.initialization_bitrate_hz = 400000U;
    config.data_bitrate_hz = 4000000U;
    config.initialization_timeout_ms = 1000U;
    config.transfer_timeout_ms = 1000U;

    for (round = 1U; round <= MTFS_RA8P1_TEST_ROUNDS; ++round) {
        mtfs_block_device_t *device;
        mtfs_block_geometry_t geometry;
        mtfs_error_t error;
        mtfs_test_t test;
        int case_result;
        int registered = 0;
        int context_ready = 0;
        int round_failure = 0;

        tm_printf((UB *)"[mtfs] round %u/%u BEGIN\n",
            round, MTFS_RA8P1_TEST_ROUNDS);
        error = mtfs_ra_sd_spi_context_init(&sd_context, &config);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[mtfs] round %u context init FAIL: mtfs=%d tk=%d fsp=%d\n",
                round, error, sd_context.last_kernel_error,
                sd_context.last_fsp_error);
            round_failure = 1;
            goto round_done;
        }
        context_ready = 1;
        device = mtfs_ra_sd_spi_block_device(&sd_context);

        error = mtfs_block_initialize(device);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[mtfs] round %u SD init FAIL: mtfs=%d tk=%d fsp=%d r1=0x%02x\n",
                round, error, sd_context.last_kernel_error,
                sd_context.last_fsp_error, sd_context.last_r1);
            round_failure = 1;
            goto round_done;
        }
        error = mtfs_block_registry_register(0U, device);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[mtfs] round %u pdrv register FAIL: %d\n",
                round, error);
            round_failure = 1;
            goto round_done;
        }
        registered = 1;

        error = mtfs_block_get_geometry(device, &geometry);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[mtfs] round %u geometry FAIL: %d\n", round, error);
            round_failure = 1;
            goto round_done;
        }
        error = mtfs_block_read(device, sector_zero, 0U, 1U);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[mtfs] round %u sector 0 read FAIL: %d\n",
                round, error);
            round_failure = 1;
            goto round_done;
        }
        tm_printf((UB *)"[mtfs] round %u SD/geometry/sector PASS: %s sectors=%u signature=%02x%02x (%s)\n",
            round,
            sd_context.card_type == MTFS_RA_SD_CARD_SDHC_SDXC
                ? (UB *)"SDHC/SDXC" : (UB *)"SDSC",
            (UW)geometry.sector_count, sector_zero[510], sector_zero[511],
            (sector_zero[510] == 0x55U && sector_zero[511] == 0xAAU)
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

round_done:
        if (registered && (mtfs_block_registry_unregister(0U) != MTFS_OK)) {
            tm_printf((UB *)"[mtfs] round %u registry cleanup FAIL\n", round);
            round_failure = 1;
        }
        if (context_ready) {
            error = mtfs_ra_sd_spi_context_deinit(&sd_context);
            if (error != MTFS_OK) {
                tm_printf((UB *)"[mtfs] round %u context cleanup FAIL: %d\n",
                    round, error);
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

    tm_printf((UB *)"[mtfs] PHASE 2.1 %s\n",
        overall_failure == 0 ? (UB *)"PASS" : (UB *)"FAIL");
    tk_exd_tsk();
}

EXPORT INT usermain(void)
{
    T_CTSK coordinator = {
        .exinf = NULL,
        .tskatr = TA_HLNG | TA_RNG3,
        .task = target_coordinator,
        .itskpri = 9,
        .stksz = 8192
    };
    ID task_id = tk_cre_tsk(&coordinator);
    if (task_id <= 0) {
        tm_printf((UB *)"[mtfs] coordinator create FAIL: %d\n", task_id);
        goto initial_task_park;
    }
    ER error = tk_sta_tsk(task_id, 0);
    if (error < E_OK) {
        tm_printf((UB *)"[mtfs] coordinator start FAIL: %d\n", error);
        (void)tk_del_tsk(task_id);
        goto initial_task_park;
    }

    /*
     * Returning from usermain() requests microT-Kernel shutdown.  Keep the
     * initial task asleep while the coordinator owns the Phase 2 test run.
     */
initial_task_park:
    for (;;) {
        (void)tk_slp_tsk(TMO_FEVR);
    }
}
