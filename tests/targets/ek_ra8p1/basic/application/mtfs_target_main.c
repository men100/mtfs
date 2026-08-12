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
    mtfs_block_device_t *device;
    mtfs_block_geometry_t geometry;
    mtfs_error_t error;
    mtfs_test_t test;
    int case_result;
    int overall_failure = 0;
    int registered = 0;
    int context_ready = 0;

    (void)start_code;
    (void)opaque;
    tm_printf((UB *)"\n[mtfs] EK-RA8P1 Phase 2 test start\n");

    config.device_name = "hspia";
    config.spi = &g_sci_spi0;
    config.ioport = &g_ioport;
    config.chip_select_pin = PMOD2_CTS;
    config.initialization_bitrate_hz = 400000U;
    config.data_bitrate_hz = 4000000U;
    config.initialization_timeout_ms = 1000U;
    config.transfer_timeout_ms = 1000U;

    error = mtfs_ra_sd_spi_context_init(&sd_context, &config);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] context init FAIL: mtfs=%d tk=%d fsp=%d\n",
            error, sd_context.last_kernel_error, sd_context.last_fsp_error);
        overall_failure = 1;
        goto done;
    }
    context_ready = 1;
    device = mtfs_ra_sd_spi_block_device(&sd_context);

    error = mtfs_block_initialize(device);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] SD init FAIL: mtfs=%d tk=%d fsp=%d r1=0x%02x\n",
            error, sd_context.last_kernel_error, sd_context.last_fsp_error,
            sd_context.last_r1);
        overall_failure = 1;
        goto done;
    }
    tm_printf((UB *)"[mtfs] SD init PASS: %s, SPI=%u Hz\n",
        sd_context.card_type == MTFS_RA_SD_CARD_SDHC_SDXC
            ? (UB *)"SDHC/SDXC" : (UB *)"SDSC",
        sd_context.current_bitrate_hz);

    error = mtfs_block_registry_register(0U, device);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] pdrv 0 register FAIL: %d\n", error);
        overall_failure = 1;
        goto done;
    }
    registered = 1;

    error = mtfs_block_get_geometry(device, &geometry);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] geometry FAIL: %d\n", error);
        overall_failure = 1;
        goto done;
    }
    tm_printf((UB *)"[mtfs] geometry PASS: sectors=%u sector_size=%u erase=%u\n",
        (UW)geometry.sector_count, geometry.sector_size, geometry.erase_block_size);

    error = mtfs_block_read(device, sector_zero, 0U, 1U);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] sector 0 read FAIL: %d\n", error);
        overall_failure = 1;
        goto done;
    }
    tm_printf((UB *)"[mtfs] sector 0 read PASS; signature=%02x %02x (%s)\n",
        sector_zero[510], sector_zero[511],
        (sector_zero[510] == 0x55U && sector_zero[511] == 0xAAU)
            ? (UB *)"present" : (UB *)"not required");

    mtfs_test_begin(&test, "fatfs_roundtrip", target_reporter, NULL);
    case_result = test_fatfs_roundtrip(&test, "0:");
    if ((mtfs_test_finish(&test) != 0) || (case_result != 0)) {
        overall_failure = 1;
    }
    if (overall_failure == 0) {
        mtfs_test_begin(&test, "fatfs_concurrent_microtkernel",
            target_reporter, NULL);
        case_result = mtfs_target_run_concurrent(&test, "0:");
        if ((mtfs_test_finish(&test) != 0) || (case_result != 0)) {
            overall_failure = 1;
        }
    }

done:
    if (registered) {
        (void)mtfs_block_registry_unregister(0U);
    }
    if (context_ready) {
        error = mtfs_ra_sd_spi_context_deinit(&sd_context);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[mtfs] cleanup FAIL: %d\n", error);
            overall_failure = 1;
        }
    }
    tm_printf((UB *)"[mtfs] PHASE 2 %s\n",
        overall_failure == 0 ? (UB *)"PASS" : (UB *)"FAIL");
    for (;;) {
        (void)tk_slp_tsk(TMO_FEVR);
    }
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
        return (INT)task_id;
    }
    ER error = tk_sta_tsk(task_id, 0);
    if (error < E_OK) {
        tm_printf((UB *)"[mtfs] coordinator start FAIL: %d\n", error);
        return (INT)error;
    }

    /*
     * Returning from usermain() requests microT-Kernel shutdown.  Keep the
     * initial task asleep while the coordinator owns the Phase 2 test run.
     */
    for (;;) {
        (void)tk_slp_tsk(TMO_FEVR);
    }
}
