#include <stddef.h>
#include <string.h>

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "ff.h"
#include "hal_data.h"
#include "mtfs_block_device.h"
#include "mtfs_block_diagnostics.h"
#include "mtfs_block_registry.h"
#include "core/mtfs_media.h"
#include "mtfs_media_service.h"
#include "mtfs_ra_sd_spi.h"
#include "mtfs_test.h"
#include "mtfs_benchmark.h"
#include "test_fatfs_lfn.h"
#include "test_fatfs_roundtrip.h"
#include "mtfs_ra8p1_platform.h"
#include "mtfs_ra8p1_vector_cache.h"
#include "mtfs_target_concurrent.h"
#include "mtfs_ra8p1_crypto_spike.h"
#include "mtfs_ra8p1_crypto_work.h"
#include "mtfs_ra8p1_model_test.h"
#ifndef MTFS_FF_FS_NORTC
#define MTFS_FF_FS_NORTC (1)
#endif
#if !MTFS_FF_FS_NORTC
#include "mtfs_ra_rtc.h"
#endif
#ifndef MTFS_TARGET_RTC_CONSOLE
#define MTFS_TARGET_RTC_CONSOLE (0)
#endif
#if !MTFS_FF_FS_NORTC && MTFS_TARGET_RTC_CONSOLE
#include "mtfs_console.h"
#include "mtfs_console_tmonitor.h"
#include "test_fatfs_timestamp.h"
#define MTFS_TARGET_COMMAND_CONSOLE_ACTIVE (1)
#else
#define MTFS_TARGET_COMMAND_CONSOLE_ACTIVE (0)
#endif

EXPORT INT usermain(void);

#define MTFS_TEST_PROFILE_SMOKE  (1)
#define MTFS_TEST_PROFILE_NORMAL (2)
#define MTFS_TEST_PROFILE_STRESS (3)

#ifndef MTFS_RA8P1_TEST_PROFILE
#define MTFS_RA8P1_TEST_PROFILE MTFS_TEST_PROFILE_NORMAL
#endif

#define MTFS_TARGET_MEDIA_INSERTED  (UINT32_C(1) << 0)
#define MTFS_TARGET_MEDIA_REMOVED   (UINT32_C(1) << 1)
#define MTFS_TARGET_MEDIA_ERROR     (UINT32_C(1) << 2)
#define MTFS_TARGET_HOTPLUG_WAIT_MS (120000U)
#define MTFS_TARGET_TEST_ROUNDS_MAX (1000U)
#define MTFS_TARGET_COORDINATOR_STACK_SIZE (16U * 1024U)
#define MTFS_TARGET_STACK_GUARD_SIZE       (32U)
#define MTFS_TARGET_STACK_FILL             (0xA5U)
#define MTFS_TARGET_STACK_PASS_FREE        (4U * 1024U)
#define MTFS_TARGET_STACK_MIN_FREE         (2U * 1024U)

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
typedef struct target_coordinator_stack
{
    UB guard[MTFS_TARGET_STACK_GUARD_SIZE];
    UW stack[MTFS_TARGET_COORDINATOR_STACK_SIZE / sizeof(UW)];
} target_coordinator_stack_t;

static target_coordinator_stack_t coordinator_stack
    __attribute__((aligned(8)));
static uint8_t sector_zero_multi[MTFS_RA_SD_SPI_SECTOR_SIZE * 2U];
#if MTFS_TARGET_COMMAND_CONSOLE_ACTIVE
static uint8_t benchmark_buffer[MTFS_BENCHMARK_BUFFER_BYTES];
#endif
static void target_media_event(void *opaque, mtfs_media_event_t event,
    mtfs_media_state_t state);
static void target_print_diagnostics(
    mtfs_ra_sd_spi_context_t *context);
static int target_run_storage_test(unsigned int rounds, int hotplug);

#if !MTFS_FF_FS_NORTC
static mtfs_ra_rtc_context_t rtc_context;
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

#if MTFS_TARGET_COMMAND_CONSOLE_ACTIVE
static int target_console_command(void *opaque, const char *line);

static int target_console_help(const char *line)
{
    const char *group;
    int all;
    int known = 0;

    if (strncmp(line, "help ", 5U) != 0) {
        return 0;
    }
    group = line + 5U;
    all = strcmp(group, "all") == 0;
    if (all || strcmp(group, "filesystem") == 0) {
        known = 1;
        mtfs_console_tmonitor_write(NULL,
            "Filesystem:\r\n"
            "  test-roundtrip [rounds]  run storage suite (default: profile)\r\n"
            "  test-fatfs-time          verify FatFs timestamp against RTC\r\n");
    }
    if (all || strcmp(group, "media") == 0) {
        known = 1;
        mtfs_console_tmonitor_write(NULL,
            "Media:\r\n"
            "  test-hotplug             run one remove/reinsert storage test\r\n");
    }
    if (all || strcmp(group, "model") == 0) {
        known = 1;
        mtfs_console_tmonitor_write(NULL,
            "Model:\r\n"
            "  model-info               authenticate and show trusted model info\r\n"
            "  model-load               load and verify MTFSTEST.MTF via model API\r\n"
            "  model-hotplug            verify removal cleanup and reinsertion recovery\r\n");
    }
    if (all || strcmp(group, "benchmark") == 0) {
        known = 1;
        mtfs_console_tmonitor_write(NULL,
            "Benchmark:\r\n"
            "  bench-info               print RA benchmark conditions\r\n"
            "  bench-smoke              run short non-destructive benchmark\r\n"
            "  bench-normal             run 1 MiB baseline benchmark\r\n");
    }
    if (all || strcmp(group, "diagnostics") == 0) {
        known = 1;
        mtfs_console_tmonitor_write(NULL,
            "Diagnostics:\r\n"
            "  diag                     print common/media/RA snapshots\r\n"
            "  diag-help                explain diagnostic commands\r\n"
            "  stack-highwater          print coordinator/worker peak stack use\r\n");
    }
    if (all || strcmp(group, "developer") == 0) {
        known = 1;
        mtfs_console_tmonitor_write(NULL,
            "Developer:\r\n"
            "  test-diagnostics-reset   verify reset on one active context\r\n"
            "  diag-reset               reset diagnostic counters only\r\n"
            "  crypto-info              show RSIP spike configuration and diagnostics\r\n"
            "  crypto-consistency       test provisioned-key GCM consistency\r\n"
            "  crypto-negative          reject SD test package tampering in RAM\r\n"
            "  crypto-package-test      verify fleet-specific SD test package\r\n"
            "  model-negative           reject reader/policy mutations via model API\r\n"
            );
    }
    if (!known) {
        mtfs_console_tmonitor_write(NULL,
            "ERROR: unknown help group\r\n"
            "Groups: general rtc filesystem media"
            " model"
            " benchmark diagnostics developer\r\n");
    }
    return 1;
}

/* Cortex-M task stacks descend toward the guard at the buffer's low end. */
static size_t target_coordinator_stack_free_bytes(void)
{
    const volatile UB *bytes =
        (const volatile UB *)coordinator_stack.stack;
    size_t free_bytes = 0U;

    while ((free_bytes < sizeof(coordinator_stack.stack)) &&
        (bytes[free_bytes] == (UB)MTFS_TARGET_STACK_FILL)) {
        ++free_bytes;
    }
    return free_bytes;
}

static int target_coordinator_stack_guard_ok(void)
{
    const volatile UB *bytes =
        (const volatile UB *)coordinator_stack.guard;
    size_t i;

    for (i = 0U; i < sizeof(coordinator_stack.guard); ++i) {
        if (bytes[i] != (UB)MTFS_TARGET_STACK_FILL) {
            return 0;
        }
    }
    return 1;
}

static const char *target_stack_watermark_status(
    const mtfs_target_stack_watermark_t *watermark)
{
    if (!watermark->measured) {
        return "INCOMPLETE";
    }
    if (!watermark->guard_ok ||
        (watermark->free_bytes < MTFS_TARGET_STACK_MIN_FREE)) {
        return "FAIL";
    }
    if (watermark->free_bytes < MTFS_TARGET_STACK_PASS_FREE) {
        return "REVIEW";
    }
    return "PASS";
}

static void target_print_stack_watermark_line(const char *name,
    const mtfs_target_stack_watermark_t *watermark)
{
    if (!watermark->measured) {
        tm_printf((UB *)"[stack] %s total=%u measured=NO status=INCOMPLETE\n",
            (UB *)name, (UW)watermark->total_bytes);
        return;
    }
    tm_printf((UB *)"[stack] %s total=%u used=%u free=%u margin=%u%% guard=%s status=%s\n",
        (UB *)name, (UW)watermark->total_bytes,
        (UW)watermark->used_bytes, (UW)watermark->free_bytes,
        (UW)((watermark->free_bytes * 100U) / watermark->total_bytes),
        watermark->guard_ok ? (UB *)"PASS" : (UB *)"FAIL",
        (UB *)target_stack_watermark_status(watermark));
}

static void target_print_stack_highwater(void)
{
    mtfs_target_stack_watermark_t watermark;
    unsigned int worker_count = mtfs_target_concurrent_stack_count();
    unsigned int i;
    int any_failure = 0;
    int any_incomplete = 0;
    int any_review = 0;

    watermark.total_bytes = sizeof(coordinator_stack.stack);
    watermark.free_bytes = target_coordinator_stack_free_bytes();
    watermark.used_bytes = watermark.total_bytes - watermark.free_bytes;
    watermark.guard_ok = target_coordinator_stack_guard_ok();
    watermark.measured = 1;
    target_print_stack_watermark_line("coordinator", &watermark);
    if (strcmp(target_stack_watermark_status(&watermark), "FAIL") == 0) {
        any_failure = 1;
    } else if (strcmp(target_stack_watermark_status(&watermark),
            "REVIEW") == 0) {
        any_review = 1;
    }

    for (i = 0U; i < worker_count; ++i) {
        char name[] = "worker-0";
        const char *status;

        name[7] = (char)('0' + i);
        if (!mtfs_target_concurrent_stack_watermark(i, &watermark)) {
            continue;
        }
        target_print_stack_watermark_line(name, &watermark);
        status = target_stack_watermark_status(&watermark);
        if (strcmp(status, "FAIL") == 0) {
            any_failure = 1;
        } else if (strcmp(status, "INCOMPLETE") == 0) {
            any_incomplete = 1;
        } else if (strcmp(status, "REVIEW") == 0) {
            any_review = 1;
        }
    }
    tm_printf((UB *)"[stack] thresholds pass-free=%u min-free=%u bytes overall=%s\n",
        (UW)MTFS_TARGET_STACK_PASS_FREE, (UW)MTFS_TARGET_STACK_MIN_FREE,
        any_failure ? (UB *)"FAIL" :
        (any_incomplete ? (UB *)"INCOMPLETE" :
        (any_review ? (UB *)"REVIEW" : (UB *)"PASS")));
}

static void target_benchmark_log(void *opaque, const char *line)
{
    (void)opaque;
    tm_printf((UB *)"%s\n", (UB *)line);
}

static void target_benchmark_info(void *opaque,
    mtfs_benchmark_log_fn log, void *log_context)
{
    const mtfs_ra_sd_spi_context_t *context =
        (const mtfs_ra_sd_spi_context_t *)opaque;
    (void)log;
    (void)log_context;
    tm_printf((UB *)"[BENCH] target card=%s transport=SPI bus_width=1 clock_hz=%u mode=blocking_irq\n",
        context->card_type == MTFS_RA_SD_CARD_SDHC_SDXC
            ? (UB *)"SDHC/SDXC" : (UB *)"SDSC",
        context->current_bitrate_hz);
    tm_printf((UB *)"[BENCH] target block_mapping=CMD17/CMD24-per-sector multi_request=split cache_i=%s cache_d=%s\n",
        (SCB->CCR & SCB_CCR_IC_Msk) ? (UB *)"enabled" : (UB *)"disabled",
        (SCB->CCR & SCB_CCR_DC_Msk) ? (UB *)"enabled" : (UB *)"disabled");
    tm_printf((UB *)"[BENCH] clock source=microtkernel_uptime+systick resolution=core_cycle tick_ms=10 monotonic=yes\n");
}

static int target_run_benchmark(
    mtfs_benchmark_profile_t profile, int info_only)
{
    mtfs_ra_sd_spi_config_t sd_config;
    mtfs_benchmark_config_t benchmark;
    mtfs_block_device_t *device = NULL;
    mtfs_error_t error;
    int failure = 1;
    int registered = 0;
    int context_ready = 0;
    int media_ready = 0;

    mtfs_ra8p1_sd_spi_config(&sd_config);
    error = mtfs_ra_sd_spi_context_init(&sd_context, &sd_config);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[BENCH] setup stage=context_init error=%d\n", error);
        goto cleanup;
    }
    context_ready = 1;
    error = mtfs_ra8p1_card_detect_start(&media_context,
        &media_service, &sd_context, target_media_event, NULL);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[BENCH] setup stage=card_detect error=%d\n", error);
        goto cleanup;
    }
    media_ready = 1;
    device = mtfs_ra_sd_spi_block_device(&sd_context);
    error = mtfs_block_initialize(device);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[BENCH] setup stage=initialize error=%d\n", error);
        goto cleanup;
    }
    error = mtfs_block_registry_register(0U, device);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[BENCH] setup stage=register error=%d\n", error);
        goto cleanup;
    }
    registered = 1;

    memset(&benchmark, 0, sizeof(benchmark));
    benchmark.device = device;
    benchmark.volume_path = "0:";
    benchmark.board_name = "EK-RA8P1";
#ifdef __OPTIMIZE__
    benchmark.build_name = "optimized";
#else
    benchmark.build_name = "debug";
#endif
    benchmark.clock_us = mtfs_ra8p1_benchmark_clock_us;
    benchmark.log = target_benchmark_log;
    benchmark.target_info = target_benchmark_info;
    benchmark.target_info_context = &sd_context;
    benchmark.buffer = benchmark_buffer;
    benchmark.buffer_size = sizeof(benchmark_buffer);
    failure = info_only
        ? mtfs_benchmark_print_info(&benchmark, profile)
        : mtfs_benchmark_run(&benchmark, profile);

cleanup:
    if (context_ready) {
        target_print_diagnostics(&sd_context);
    }
    if (media_ready && (mtfs_ra8p1_card_detect_stop() != MTFS_OK)) {
        tm_printf((UB *)"[BENCH] cleanup stage=card_detect status=FAIL\n");
        failure = 1;
    }
    if (registered && (mtfs_block_registry_unregister(0U) != MTFS_OK)) {
        tm_printf((UB *)"[BENCH] cleanup stage=registry status=FAIL\n");
        failure = 1;
    }
    if (context_ready &&
        (mtfs_ra_sd_spi_context_deinit(&sd_context) != MTFS_OK)) {
        tm_printf((UB *)"[BENCH] cleanup stage=context status=FAIL\n");
        failure = 1;
    }
    tm_printf((UB *)"[BENCH] COMMAND status=%s\n",
        failure ? (UB *)"FAIL" : (UB *)"PASS");
    return failure;
}

static void target_command_console(void)
{
    mtfs_console_t console;
    mtfs_console_init(&console, mtfs_console_tmonitor_write, NULL);
    mtfs_console_set_extension(&console, target_console_command, &console,
        "  filesystem\r\n  media\r\n"
        "  model\r\n"
        "  benchmark\r\n"
        "  diagnostics\r\n  developer\r\n"
        "Use help <group> for details.\r\n");
    mtfs_console_tmonitor_write(NULL,
        "microT-FS EK-RA8P1 command console\r\n"
        "Commands: RTC, storage tests, benchmark, and crypto diagnostics.\r\n"
        "RTC set uses local time; no timezone/DST conversion.\r\n"
        "Type help for commands.\r\n> ");
    for (;;) {
        mtfs_console_feed(&console,
            (char)mtfs_console_tmonitor_getchar());
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
    mtfs_ra8p1_card_detect_diagnostics_t cd;
    mtfs_media_diagnostics_t media_diagnostics;
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
    (void)mtfs_media_diagnostics_get(&media_context, &media_diagnostics);
    tm_printf((UB *)"[mtfs] CD raw=%u active=%s irq=%u rise=%u fall=%u debounce=%u/%u events=%u/%u/%u\n",
        cd.raw_level, cd.active_low ? (UB *)"low" : (UB *)"high",
        cd.irq_entries, cd.rising_edges, cd.falling_edges,
        media_diagnostics.debounce_starts,
        media_diagnostics.debounce_rechecks,
        media_inserted_events, media_removed_events, media_error_events);
}

#define MTFS_TARGET_MEDIA_WAIT_SLICE_MS (100U)
#define MTFS_TARGET_MEDIA_WAIT_LOG_MS   (5000U)
#define MTFS_TARGET_IRQ_GRACE_MS        (500U)

static void target_print_card_detect_hardware(void)
{
    mtfs_ra8p1_card_detect_hardware_diagnostics_t hardware;

    mtfs_ra8p1_get_card_detect_hardware_diagnostics(&hardware);
    tm_printf((UB *)"[mtfs] CD HW irq=%d pfs(P000/P409)=0x%08x/0x%08x irqcr=0x%02x "
        "ielsr=0x%08x nvic=%u/%u vector=0x%08x expected=0x%08x\n",
        hardware.vector_number, hardware.p000_pfs, hardware.p409_pfs,
        hardware.irqcr,
        hardware.ielsr, hardware.nvic_enabled, hardware.nvic_pending,
        hardware.vector_entry, hardware.expected_vector_entry);
}

static int target_wait_media_event(UINT expected, const char *operation)
{
    mtfs_ra8p1_card_detect_diagnostics_t initial;
    uint32_t elapsed_ms = 0U;
    uint32_t expected_raw_ms = 0U;
    uint32_t next_log_ms = MTFS_TARGET_MEDIA_WAIT_LOG_MS;
    int expected_present = (expected == MTFS_TARGET_MEDIA_INSERTED);

    mtfs_ra8p1_get_card_detect_diagnostics(&initial);
    while (elapsed_ms < MTFS_TARGET_HOTPLUG_WAIT_MS) {
        UINT events = 0U;
        uint32_t remaining_ms = MTFS_TARGET_HOTPLUG_WAIT_MS - elapsed_ms;
        uint32_t wait_ms = remaining_ms < MTFS_TARGET_MEDIA_WAIT_SLICE_MS
            ? remaining_ms : MTFS_TARGET_MEDIA_WAIT_SLICE_MS;
        ER result = tk_wai_flg(media_application_event_flag_id,
            expected | MTFS_TARGET_MEDIA_ERROR, TWF_ORW | TWF_BITCLR,
            &events, (TMO)wait_ms);

        if (result >= E_OK) {
            if ((events & expected) != 0U) {
                return 1;
            }
            tm_printf((UB *)"[mtfs] %s wait FAIL tk=%d events=0x%08x\n",
                (UB *)operation, result, events);
            return 0;
        }
        if (MERCD(result) != MERCD(E_TMOUT)) {
            tm_printf((UB *)"[mtfs] %s wait FAIL tk=%d events=0x%08x\n",
                (UB *)operation, result, events);
            return 0;
        }

        elapsed_ms += wait_ms;
        {
            mtfs_ra8p1_card_detect_diagnostics_t current;
            int present;

            mtfs_ra8p1_get_card_detect_diagnostics(&current);
            present = current.active_low ? !current.raw_level
                                         : current.raw_level;
            if (present == expected_present) {
                expected_raw_ms += wait_ms;
                if ((current.irq_entries == initial.irq_entries) &&
                    (expected_raw_ms >= MTFS_TARGET_IRQ_GRACE_MS)) {
                    tm_printf((UB *)"[mtfs] %s IRQ DELIVERY FAIL: "
                        "GPIO raw=%u reached expected %s but IRQ count "
                        "remained %u for %u ms\n",
                        (UB *)operation, current.raw_level,
                        expected_present ? (UB *)"PRESENT" : (UB *)"ABSENT",
                        current.irq_entries, expected_raw_ms);
                    target_print_card_detect_hardware();
                    return 0;
                }
            } else {
                expected_raw_ms = 0U;
            }
            if (elapsed_ms >= next_log_ms) {
                tm_printf((UB *)"[mtfs] WAITING for %s: raw=%u irq=%u "
                    "rise=%u fall=%u elapsed=%u/%u ms\n",
                    (UB *)operation, current.raw_level,
                    current.irq_entries, current.rising_edges,
                    current.falling_edges, elapsed_ms,
                    MTFS_TARGET_HOTPLUG_WAIT_MS);
                next_log_ms += MTFS_TARGET_MEDIA_WAIT_LOG_MS;
            }
        }
    }
    tm_printf((UB *)"[mtfs] %s wait FAIL tk=%d events=0x%08x\n",
        (UB *)operation, E_TMOUT, 0U);
    target_print_card_detect_hardware();
    return 0;
}

static int target_check_spi_diagnostics(
    mtfs_test_t *test, mtfs_ra_sd_spi_context_t *context)
{
    mtfs_ra_sd_spi_diagnostics_t snapshot;
    const mtfs_ra_sd_spi_diagnostics_t *diagnostics = &snapshot;

    if (!MTFS_TEST_CHECK(test,
            mtfs_ra_sd_spi_diagnostics_get(context, &snapshot) == MTFS_OK,
            "typed SD SPI diagnostics snapshot is available")) {
        return 1;
    }

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
    (void)MTFS_TEST_CHECK(test, diagnostics->token_timeouts == 0U,
        "SD data-token waits did not time out");
    (void)MTFS_TEST_CHECK(test, diagnostics->ready_timeouts == 0U,
        "SD ready waits did not time out");
    (void)MTFS_TEST_CHECK(test, diagnostics->monotonic_clock_errors == 0U,
        "monotonic clock reads did not fail");
    (void)MTFS_TEST_CHECK(test, diagnostics->cmd0_timeouts == 0U,
        "CMD0 initialization did not time out");
    return test->failures == 0U ? 0 : 1;
}

static const char *target_sd_init_stage_name(
    mtfs_ra_sd_spi_init_stage_t stage)
{
    switch (stage) {
    case MTFS_RA_SD_SPI_INIT_NONE:
        return "none";
    case MTFS_RA_SD_SPI_INIT_SPI_OPEN:
        return "spi-open";
    case MTFS_RA_SD_SPI_INIT_POWER_UP:
        return "power-up";
    case MTFS_RA_SD_SPI_INIT_CMD0:
        return "cmd0";
    case MTFS_RA_SD_SPI_INIT_CMD8:
        return "cmd8";
    case MTFS_RA_SD_SPI_INIT_ACMD41:
        return "acmd41";
    case MTFS_RA_SD_SPI_INIT_CMD58:
        return "cmd58";
    case MTFS_RA_SD_SPI_INIT_CSD:
        return "csd";
    case MTFS_RA_SD_SPI_INIT_DATA_RATE:
        return "data-rate";
    case MTFS_RA_SD_SPI_INIT_COMPLETE:
        return "complete";
    default:
        return "unknown";
    }
}

static void target_print_diagnostics(
    mtfs_ra_sd_spi_context_t *context)
{
    mtfs_ra_sd_spi_diagnostics_t port_snapshot;
    mtfs_block_diagnostics_t common;
    mtfs_media_diagnostics_t media;
    const mtfs_ra_sd_spi_diagnostics_t *diagnostics = &port_snapshot;

    if (mtfs_ra_sd_spi_diagnostics_get(context, &port_snapshot) != MTFS_OK) {
        tm_printf((UB *)"[diag] RA SD SPI snapshot unavailable\n");
        return;
    }
    if (mtfs_block_diagnostics_get(
            mtfs_ra_sd_spi_block_device(context), &common) == MTFS_OK) {
        tm_printf((UB *)"[diag] common v=%u size=%u epoch=%u op=%u error=%d status=0x%08x geometry=%u/0x%08x%08x/%u\n",
            common.api_version, common.struct_size, common.reset_epoch,
            common.last_operation, common.last_error, common.status,
            common.sector_size, (UW)(common.sector_count >> 32U),
            (UW)common.sector_count,
            common.erase_block_size);
        tm_printf((UB *)"[diag] common calls init=%u read=%u/%u/%u write=%u/%u/%u sync=%u/%u/%u sectors=%u/%u/%u/%u\n",
            common.initialize_calls, common.read_calls, common.read_successes,
            common.read_failures, common.write_calls, common.write_successes,
            common.write_failures, common.sync_calls, common.sync_successes,
            common.sync_failures, common.read_sectors_requested,
            common.read_sectors_completed, common.write_sectors_requested,
            common.write_sectors_completed);
    }
    if (mtfs_media_diagnostics_get(&media_context, &media) == MTFS_OK) {
        tm_printf((UB *)"[diag] media v=%u epoch=%u state=%u present=%u sequence=%u generation=%u irq=%u manual=%u poll=%u debounce=%u/%u events=%u/%u/%u\n",
            media.api_version, media.reset_epoch, media.state,
            media.stable_present, media.notification_sequence,
            media.media_generation, media.irq_notifications,
            media.manual_notifications, media.poll_checks,
            media.debounce_starts, media.debounce_rechecks,
            media.inserted_events, media.removed_events, media.error_events);
    }

    tm_printf((UB *)"[mtfs] spi starts=%u complete=%u errors=%u read=%u write=%u\n",
        diagnostics->transfer_starts,
        diagnostics->transfer_completions,
        diagnostics->transfer_errors,
        diagnostics->read_sectors,
        diagnostics->write_sectors);
    tm_printf((UB *)"[mtfs] media removal hints=%u wait wakeups=%u\n",
        diagnostics->media_removal_notifications,
        diagnostics->media_wait_wakeups);
    tm_printf((UB *)"[mtfs] wait token calls=%u polls=%u max=%u timeouts=%u\n",
        diagnostics->token_wait_calls,
        diagnostics->token_poll_bytes,
        diagnostics->token_max_polls,
        diagnostics->token_timeouts);
    tm_printf((UB *)"[mtfs] wait ready calls=%u polls=%u max=%u timeouts=%u\n",
        diagnostics->ready_wait_calls,
        diagnostics->ready_poll_bytes,
        diagnostics->ready_max_polls,
        diagnostics->ready_timeouts);
    tm_printf((UB *)"[mtfs] init stage=%s cmd0_attempts=%u no_response=%u ready_response=%u timeouts=%u\n",
        (UB *)target_sd_init_stage_name(diagnostics->initialization_stage),
        diagnostics->cmd0_attempts,
        diagnostics->cmd0_no_response,
        diagnostics->cmd0_ready_responses,
        diagnostics->cmd0_timeouts);
    tm_printf((UB *)"[mtfs] init acmd41_retries=%u monotonic_clock_errors=%u\n",
        diagnostics->acmd41_retries,
        diagnostics->monotonic_clock_errors);
    tm_printf((UB *)"[mtfs] last mtfs=%d tk=%d fsp=%d r1=0x%02x bitrate=%u\n",
        diagnostics->last_error, diagnostics->last_kernel_error,
        diagnostics->last_fsp_error, diagnostics->last_r1,
        diagnostics->current_bitrate_hz);
}

#if MTFS_TARGET_COMMAND_CONSOLE_ACTIVE
static int target_common_counters_are_clear(
    const mtfs_block_diagnostics_t *diagnostics)
{
    return (diagnostics->initialize_calls == 0U) &&
        (diagnostics->initialize_successes == 0U) &&
        (diagnostics->initialize_failures == 0U) &&
        (diagnostics->status_calls == 0U) &&
        (diagnostics->status_failures == 0U) &&
        (diagnostics->read_calls == 0U) &&
        (diagnostics->read_successes == 0U) &&
        (diagnostics->read_failures == 0U) &&
        (diagnostics->write_calls == 0U) &&
        (diagnostics->write_successes == 0U) &&
        (diagnostics->write_failures == 0U) &&
        (diagnostics->sync_calls == 0U) &&
        (diagnostics->sync_successes == 0U) &&
        (diagnostics->sync_failures == 0U) &&
        (diagnostics->geometry_calls == 0U) &&
        (diagnostics->geometry_failures == 0U) &&
        (diagnostics->trim_calls == 0U) &&
        (diagnostics->trim_successes == 0U) &&
        (diagnostics->trim_failures == 0U) &&
        (diagnostics->read_sectors_requested == 0U) &&
        (diagnostics->read_sectors_completed == 0U) &&
        (diagnostics->write_sectors_requested == 0U) &&
        (diagnostics->write_sectors_completed == 0U) &&
        (diagnostics->io_errors == 0U) &&
        (diagnostics->not_ready_errors == 0U) &&
        (diagnostics->no_media_errors == 0U) &&
        (diagnostics->write_protected_errors == 0U) &&
        (diagnostics->out_of_range_errors == 0U) &&
        (diagnostics->timeout_errors == 0U) &&
        (diagnostics->other_errors == 0U);
}

static int target_media_counters_are_clear(
    const mtfs_media_diagnostics_t *diagnostics)
{
    return (diagnostics->irq_notifications == 0U) &&
        (diagnostics->manual_notifications == 0U) &&
        (diagnostics->poll_checks == 0U) &&
        (diagnostics->debounce_starts == 0U) &&
        (diagnostics->debounce_rechecks == 0U) &&
        (diagnostics->inserted_events == 0U) &&
        (diagnostics->removed_events == 0U) &&
        (diagnostics->error_events == 0U);
}

static int target_ra_counters_are_clear(
    const mtfs_ra_sd_spi_diagnostics_t *diagnostics)
{
    return (diagnostics->transfer_starts == 0U) &&
        (diagnostics->transfer_completions == 0U) &&
        (diagnostics->transfer_errors == 0U) &&
        (diagnostics->media_removal_notifications == 0U) &&
        (diagnostics->media_wait_wakeups == 0U) &&
        (diagnostics->read_sectors == 0U) &&
        (diagnostics->write_sectors == 0U) &&
        (diagnostics->token_wait_calls == 0U) &&
        (diagnostics->token_poll_bytes == 0U) &&
        (diagnostics->token_max_polls == 0U) &&
        (diagnostics->token_timeouts == 0U) &&
        (diagnostics->ready_wait_calls == 0U) &&
        (diagnostics->ready_poll_bytes == 0U) &&
        (diagnostics->ready_max_polls == 0U) &&
        (diagnostics->ready_timeouts == 0U) &&
        (diagnostics->acmd41_retries == 0U) &&
        (diagnostics->monotonic_clock_errors == 0U) &&
        (diagnostics->cmd0_attempts == 0U) &&
        (diagnostics->cmd0_no_response == 0U) &&
        (diagnostics->cmd0_ready_responses == 0U) &&
        (diagnostics->cmd0_timeouts == 0U);
}

static int target_run_diagnostics_reset_test(void)
{
    mtfs_ra_sd_spi_config_t config;
    mtfs_block_device_t *device = NULL;
    mtfs_block_geometry_t geometry;
    mtfs_block_status_t status = 0U;
    mtfs_block_diagnostics_t common_before;
    mtfs_block_diagnostics_t common_reset;
    mtfs_block_diagnostics_t common_after;
    mtfs_media_diagnostics_t media_before;
    mtfs_media_diagnostics_t media_reset;
    mtfs_media_diagnostics_t media_after;
    mtfs_ra_sd_spi_diagnostics_t port_before;
    mtfs_ra_sd_spi_diagnostics_t port_reset;
    mtfs_ra_sd_spi_diagnostics_t port_after;
    mtfs_test_t test;
    mtfs_error_t error;
    int registered = 0;
    int context_ready = 0;
    int media_ready = 0;
    int failure;

    mtfs_test_begin(&test, "diagnostics_reset_active_context",
        target_reporter, NULL);
    mtfs_ra8p1_sd_spi_config(&config);
    error = mtfs_ra_sd_spi_context_init(&sd_context, &config);
    if (!MTFS_TEST_CHECK(&test, error == MTFS_OK,
            "RA context initialization succeeds")) goto cleanup;
    context_ready = 1;

    error = mtfs_ra8p1_card_detect_start(&media_context,
        &media_service, &sd_context, target_media_event, NULL);
    if (!MTFS_TEST_CHECK(&test, error == MTFS_OK,
            "media context starts")) goto cleanup;
    media_ready = 1;
    device = mtfs_ra_sd_spi_block_device(&sd_context);
    error = mtfs_block_initialize(device);
    if (!MTFS_TEST_CHECK(&test, error == MTFS_OK,
            "block device initializes")) goto cleanup;
    error = mtfs_block_registry_register(0U, device);
    if (!MTFS_TEST_CHECK(&test, error == MTFS_OK,
            "block device registers")) goto cleanup;
    registered = 1;
    error = mtfs_block_get_geometry(device, &geometry);
    if (!MTFS_TEST_CHECK(&test, error == MTFS_OK,
            "geometry query succeeds")) goto cleanup;
    (void)MTFS_TEST_CHECK(&test,
        (geometry.sector_size == MTFS_RA_SD_SPI_SECTOR_SIZE) &&
            (geometry.sector_count > 0U),
        "geometry is usable before reset");
    error = mtfs_block_status(device, &status);
    if (!MTFS_TEST_CHECK(&test, error == MTFS_OK,
            "status query succeeds")) goto cleanup;
    (void)MTFS_TEST_CHECK(&test,
        (status & (MTFS_BLOCK_STATUS_INITIALIZED |
            MTFS_BLOCK_STATUS_MEDIA_PRESENT)) ==
            (MTFS_BLOCK_STATUS_INITIALIZED |
                MTFS_BLOCK_STATUS_MEDIA_PRESENT),
        "status reports initialized media before reset");
    error = mtfs_block_read(device, sector_zero_single, 0U, 1U);
    if (!MTFS_TEST_CHECK(&test, error == MTFS_OK,
            "raw read succeeds before reset")) goto cleanup;

    if (!MTFS_TEST_CHECK(&test,
            mtfs_block_diagnostics_get(device, &common_before) == MTFS_OK,
            "common snapshot is available before reset")) goto cleanup;
    if (!MTFS_TEST_CHECK(&test,
            mtfs_media_diagnostics_get(&media_context, &media_before) ==
                MTFS_OK,
            "media snapshot is available before reset")) goto cleanup;
    if (!MTFS_TEST_CHECK(&test,
            mtfs_ra_sd_spi_diagnostics_get(&sd_context, &port_before) ==
                MTFS_OK,
            "RA snapshot is available before reset")) goto cleanup;
    (void)MTFS_TEST_CHECK(&test,
        (common_before.read_calls > 0U) &&
            (common_before.read_sectors_completed > 0U) &&
            (port_before.read_sectors > 0U),
        "I/O counters increase before reset");

    error = mtfs_block_diagnostics_reset(device);
    (void)MTFS_TEST_CHECK(&test, error == MTFS_OK,
        "common diagnostics reset succeeds");
    error = mtfs_media_diagnostics_reset(&media_context);
    (void)MTFS_TEST_CHECK(&test, error == MTFS_OK,
        "media diagnostics reset succeeds");
    error = mtfs_ra_sd_spi_diagnostics_reset(&sd_context);
    (void)MTFS_TEST_CHECK(&test, error == MTFS_OK,
        "RA diagnostics reset succeeds");

    if (!MTFS_TEST_CHECK(&test,
            mtfs_block_diagnostics_get(device, &common_reset) == MTFS_OK,
            "common snapshot is available after reset")) goto cleanup;
    if (!MTFS_TEST_CHECK(&test,
            mtfs_media_diagnostics_get(&media_context, &media_reset) ==
                MTFS_OK,
            "media snapshot is available after reset")) goto cleanup;
    if (!MTFS_TEST_CHECK(&test,
            mtfs_ra_sd_spi_diagnostics_get(&sd_context, &port_reset) ==
                MTFS_OK,
            "RA snapshot is available after reset")) goto cleanup;

    (void)MTFS_TEST_CHECK(&test,
        common_reset.reset_epoch == common_before.reset_epoch + 1U,
        "common reset epoch advances");
    (void)MTFS_TEST_CHECK(&test,
        media_reset.reset_epoch == media_before.reset_epoch + 1U,
        "media reset epoch advances");
    (void)MTFS_TEST_CHECK(&test,
        port_reset.reset_epoch == port_before.reset_epoch + 1U,
        "RA reset epoch advances");
    (void)MTFS_TEST_CHECK(&test,
        target_common_counters_are_clear(&common_reset),
        "all common counters clear");
    (void)MTFS_TEST_CHECK(&test,
        target_media_counters_are_clear(&media_reset),
        "all media counters clear");
    (void)MTFS_TEST_CHECK(&test,
        target_ra_counters_are_clear(&port_reset),
        "all RA counters clear");
    (void)MTFS_TEST_CHECK(&test,
        (common_reset.validity_mask == common_before.validity_mask) &&
            (common_reset.flags == common_before.flags) &&
            (common_reset.capabilities == common_before.capabilities) &&
            (common_reset.status == common_before.status) &&
            (common_reset.sector_size == common_before.sector_size) &&
            (common_reset.sector_count == common_before.sector_count) &&
            (common_reset.erase_block_size ==
                common_before.erase_block_size) &&
            (common_reset.last_operation == common_before.last_operation) &&
            (common_reset.last_error == common_before.last_error),
        "common cached state is preserved");
    (void)MTFS_TEST_CHECK(&test,
        ((common_reset.status & (MTFS_BLOCK_STATUS_INITIALIZED |
            MTFS_BLOCK_STATUS_MEDIA_PRESENT)) ==
            (MTFS_BLOCK_STATUS_INITIALIZED |
                MTFS_BLOCK_STATUS_MEDIA_PRESENT)) &&
            ((common_reset.validity_mask &
                MTFS_BLOCK_DIAGNOSTICS_VALID_GEOMETRY) != 0U),
        "common active status and geometry remain valid");
    (void)MTFS_TEST_CHECK(&test,
        (media_reset.validity_mask == media_before.validity_mask) &&
            (media_reset.media_generation == media_before.media_generation) &&
            (media_reset.notification_sequence ==
                media_before.notification_sequence) &&
            (media_reset.state == media_before.state) &&
            (media_reset.stable_present == media_before.stable_present) &&
            (media_reset.state == MTFS_MEDIA_STATE_PRESENT) &&
            (media_reset.stable_present != 0U),
        "media state and generation are preserved");
    (void)MTFS_TEST_CHECK(&test,
        (port_reset.validity_mask == port_before.validity_mask) &&
            (port_reset.initialization_stage ==
                port_before.initialization_stage) &&
            (port_reset.card_type == port_before.card_type) &&
            (port_reset.current_bitrate_hz ==
                port_before.current_bitrate_hz) &&
            (port_reset.last_fsp_error == port_before.last_fsp_error) &&
            (port_reset.last_kernel_error == port_before.last_kernel_error) &&
            (port_reset.last_error == port_before.last_error) &&
            (port_reset.last_r1 == port_before.last_r1) &&
            (port_reset.initialized == port_before.initialized) &&
            (port_reset.fsp_open == port_before.fsp_open) &&
            (port_reset.media_removal_pending ==
                port_before.media_removal_pending) &&
            (port_reset.initialized != 0U) &&
            (port_reset.fsp_open != 0U),
        "RA cached and live state are preserved");

    error = mtfs_block_read(device, sector_zero_multi, 0U, 1U);
    (void)MTFS_TEST_CHECK(&test, error == MTFS_OK,
        "raw read succeeds on the same context after reset");
    (void)MTFS_TEST_CHECK(&test,
        memcmp(sector_zero_single, sector_zero_multi,
            MTFS_RA_SD_SPI_SECTOR_SIZE) == 0,
        "raw data is unchanged after reset");
    if (!MTFS_TEST_CHECK(&test,
            mtfs_block_diagnostics_get(device, &common_after) == MTFS_OK,
            "common snapshot is available after post-reset I/O")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(&test,
            mtfs_media_diagnostics_get(&media_context, &media_after) ==
                MTFS_OK,
            "media snapshot is available after post-reset I/O")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(&test,
            mtfs_ra_sd_spi_diagnostics_get(&sd_context, &port_after) ==
                MTFS_OK,
            "RA snapshot is available after post-reset I/O")) goto cleanup;
    (void)MTFS_TEST_CHECK(&test,
        (common_after.reset_epoch == common_reset.reset_epoch) &&
            (common_after.read_calls == 1U) &&
            (common_after.read_successes == 1U) &&
            (common_after.read_failures == 0U) &&
            (common_after.read_sectors_requested == 1U) &&
            (common_after.read_sectors_completed == 1U),
        "common read counters restart from zero");
    (void)MTFS_TEST_CHECK(&test,
        (media_after.reset_epoch == media_reset.reset_epoch) &&
            (media_after.media_generation == media_reset.media_generation) &&
            (media_after.state == MTFS_MEDIA_STATE_PRESENT) &&
            (media_after.stable_present != 0U),
        "media state remains active after post-reset I/O");
    (void)MTFS_TEST_CHECK(&test,
        (port_after.reset_epoch == port_reset.reset_epoch) &&
            (port_after.read_sectors == 1U) &&
            (port_after.transfer_starts > 0U) &&
            (port_after.transfer_starts ==
                port_after.transfer_completions) &&
            (port_after.transfer_errors == 0U),
        "RA transfer counters restart from zero");

cleanup:
    if (context_ready) target_print_diagnostics(&sd_context);
    if (media_ready) {
        error = mtfs_ra8p1_card_detect_stop();
        (void)MTFS_TEST_CHECK(&test, error == MTFS_OK,
            "media context cleanup succeeds");
    }
    if (registered) {
        error = mtfs_block_registry_unregister(0U);
        (void)MTFS_TEST_CHECK(&test, error == MTFS_OK,
            "block registry cleanup succeeds");
    }
    if (context_ready) {
        error = mtfs_ra_sd_spi_context_deinit(&sd_context);
        (void)MTFS_TEST_CHECK(&test, error == MTFS_OK,
            "RA context cleanup succeeds");
    }
    failure = mtfs_test_finish(&test) != 0;
    tm_printf((UB *)"[mtfs] diagnostics reset command %s\n",
        failure ? (UB *)"FAIL" : (UB *)"PASS");
    return failure ? 1 : 0;
}

static void target_print_timestamp_datetime(
    const char *label, const mtfs_datetime_t *datetime)
{
    tm_printf((UB *)"%s%04u-%02u-%02u %02u:%02u:%02u\n",
        (UB *)label, datetime->year, datetime->month, datetime->day,
        datetime->hour, datetime->minute, datetime->second);
}

static int target_run_fatfs_time_test(void)
{
    mtfs_ra_sd_spi_config_t config;
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

    memset(&timestamp_result, 0, sizeof(timestamp_result));
    if ((mtfs_time_get_status(&rtc_status) != MTFS_OK) ||
        (rtc_status != MTFS_TIME_STATUS_VALID)) {
        tm_printf((UB *)"[mtfs] FAT timestamp command FAIL: RTC state=%u\n",
            (UW)rtc_status);
        return 1;
    }
    mtfs_ra8p1_sd_spi_config(&config);
    error = mtfs_ra_sd_spi_context_init(&sd_context, &config);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] timestamp context init FAIL: %d\n", error);
        goto cleanup;
    }
    context_ready = 1;
    error = mtfs_ra8p1_card_detect_start(&media_context,
        &media_service, &sd_context, target_media_event, NULL);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] timestamp card detect start FAIL: %d\n",
            error);
        goto cleanup;
    }
    media_ready = 1;
    device = mtfs_ra_sd_spi_block_device(&sd_context);
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
    if (media_ready && (mtfs_ra8p1_card_detect_stop() != MTFS_OK)) {
        tm_printf((UB *)"[mtfs] timestamp card detect cleanup FAIL\n");
        test_failure = 1;
    }
    if (registered && (mtfs_block_registry_unregister(0U) != MTFS_OK)) {
        tm_printf((UB *)"[mtfs] timestamp registry cleanup FAIL\n");
        test_failure = 1;
    }
    if (context_ready &&
        (mtfs_ra_sd_spi_context_deinit(&sd_context) != MTFS_OK)) {
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

static int target_console_command(void *opaque, const char *line)
{
    static const char command[] = "test-roundtrip";
    unsigned int rounds = MTFS_RA8P1_TEST_ROUNDS;
    size_t offset = sizeof(command) - 1U;
    int parse_result = 0;

    (void)opaque;
    if (target_console_help(line)) {
        return 1;
    }
    if (strcmp(line, command) == 0) {
        parse_result = 1;
    } else if ((strncmp(line, command, offset) == 0) &&
        (line[offset] == ' ')) {
        const char *argument = line + offset + 1U;
        rounds = 0U;
        parse_result = -1;
        while ((*argument >= '0') && (*argument <= '9')) {
            unsigned int digit = (unsigned int)(*argument++ - '0');
            if (rounds > ((MTFS_TARGET_TEST_ROUNDS_MAX - digit) / 10U)) {
                rounds = MTFS_TARGET_TEST_ROUNDS_MAX + 1U;
                break;
            }
            rounds = rounds * 10U + digit;
            parse_result = 1;
        }
        if ((*argument != '\0') || (rounds == 0U) ||
            (rounds > MTFS_TARGET_TEST_ROUNDS_MAX)) {
            parse_result = -1;
        }
    }
    if (parse_result != 0) {
        if (parse_result < 0) {
            tm_printf((UB *)"[mtfs] usage: test-roundtrip [1..%u] (default=%u)\n",
                MTFS_TARGET_TEST_ROUNDS_MAX, MTFS_RA8P1_TEST_ROUNDS);
        } else {
            (void)target_run_storage_test(rounds, 0);
        }
        return 1;
    }
    if (strcmp(line, "test-hotplug") == 0) {
        (void)target_run_storage_test(1U, 1);
        return 1;
    }
    if ((strcmp(line, "crypto-package-test") == 0) ||
        (strcmp(line, "crypto-negative") == 0) ||
        (strcmp(line, "model-info") == 0) ||
        (strcmp(line, "model-load") == 0) ||
        (strcmp(line, "model-negative") == 0) ||
        (strcmp(line, "model-hotplug") == 0)) {
        mtfs_ra_sd_spi_config_t config;
        mtfs_block_device_t *device = NULL;
        mtfs_error_t error;
        int registered = 0;
        int context_ready = 0;
        int media_ready = 0;

        mtfs_ra8p1_sd_spi_config(&config);
        error = mtfs_ra_sd_spi_context_init(&sd_context, &config);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[crypto] SD setup FAIL stage=context mtfs=%d\n",
                error);
            goto crypto_sd_cleanup;
        }
        context_ready = 1;
        error = mtfs_ra8p1_card_detect_start(&media_context,
            &media_service, &sd_context, target_media_event, NULL);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[crypto] SD setup FAIL stage=card-detect mtfs=%d\n",
                error);
            goto crypto_sd_cleanup;
        }
        media_ready = 1;
        device = mtfs_ra_sd_spi_block_device(&sd_context);
        error = mtfs_block_initialize(device);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[crypto] SD setup FAIL stage=initialize mtfs=%d\n",
                error);
            goto crypto_sd_cleanup;
        }
        error = mtfs_block_registry_register(0U, device);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[crypto] SD setup FAIL stage=register mtfs=%d\n",
                error);
            goto crypto_sd_cleanup;
        }
        registered = 1;
        if (!mtfs_ra8p1_model_command(line, &media_context, device,
                &registered))
            (void)mtfs_ra8p1_crypto_spike_command(line);

crypto_sd_cleanup:
        (void)f_mount(NULL, "0:", 0U);
        if (media_ready &&
            (mtfs_ra8p1_card_detect_stop() != MTFS_OK)) {
            tm_printf((UB *)"[crypto] SD cleanup FAIL stage=card-detect\n");
        }
        if (registered &&
            (mtfs_block_registry_unregister(0U) != MTFS_OK)) {
            tm_printf((UB *)"[crypto] SD cleanup FAIL stage=registry\n");
        }
        if (context_ready &&
            (mtfs_ra_sd_spi_context_deinit(&sd_context) != MTFS_OK)) {
            tm_printf((UB *)"[crypto] SD cleanup FAIL stage=context\n");
        }
        return 1;
    }
    if (mtfs_ra8p1_crypto_spike_command(line)) {
        return 1;
    }
    if (strcmp(line, "test-fatfs-time") == 0) {
        (void)target_run_fatfs_time_test();
        return 1;
    }
    if (strcmp(line, "bench-info") == 0) {
        (void)target_run_benchmark(MTFS_BENCHMARK_PROFILE_NORMAL, 1);
        return 1;
    }
    if (strcmp(line, "bench-smoke") == 0) {
        (void)target_run_benchmark(MTFS_BENCHMARK_PROFILE_SMOKE, 0);
        return 1;
    }
    if (strcmp(line, "bench-normal") == 0) {
        (void)target_run_benchmark(MTFS_BENCHMARK_PROFILE_NORMAL, 0);
        return 1;
    }
    if (strcmp(line, "test-diagnostics-reset") == 0) {
        (void)target_run_diagnostics_reset_test();
        return 1;
    }
    if (strcmp(line, "diag") == 0) {
        target_print_diagnostics(&sd_context);
        return 1;
    }
    if (strcmp(line, "diag-reset") == 0) {
        mtfs_error_t common = mtfs_block_diagnostics_reset(
            mtfs_ra_sd_spi_block_device(&sd_context));
        mtfs_error_t media = mtfs_media_diagnostics_reset(&media_context);
        mtfs_error_t port = mtfs_ra_sd_spi_diagnostics_reset(&sd_context);
        tm_printf((UB *)"[diag] reset common=%d media=%d ra=%d (I/O/media state unchanged)\n",
            common, media, port);
        return 1;
    }
    if (strcmp(line, "diag-help") == 0) {
        tm_printf((UB *)"[diag] diag reads cached snapshots from the last storage command without media I/O; multi-round counters are cumulative and live state is inactive after cleanup; diag-reset clears counters and advances reset epochs only\n");
        return 1;
    }
    if (strcmp(line, "stack-highwater") == 0) {
        target_print_stack_highwater();
        return 1;
    }
    return 0;
}
#endif

static int target_run_storage_test(unsigned int rounds, int hotplug)
{
    mtfs_ra_sd_spi_config_t config;
    mtfs_block_device_t *device = NULL;
    mtfs_error_t error = MTFS_OK;
    unsigned int round;
    int registered = 0;
    int context_ready = 0;
    int media_ready = 0;
    int overall_failure = 0;
    T_CFLG media_flag_config = {
        .flgatr = TA_TFIFO,
        .iflgptn = 0U
    };

    media_inserted_events = 0U;
    media_removed_events = 0U;
    media_error_events = 0U;
    media_reinitialize_count = 0U;
    mtfs_ra8p1_sd_spi_config(&config);
    if (hotplug) {
        media_application_event_flag_id = tk_cre_flg(&media_flag_config);
        if (media_application_event_flag_id <= 0) {
            tm_printf((UB *)"[mtfs] application media event flag create FAIL: %d\n",
                media_application_event_flag_id);
            media_application_event_flag_id = 0;
            return 1;
        }
    }

    tm_printf((UB *)"\n[mtfs] EK-RA8P1 storage test: profile=%s rounds=%u path=SCI_B SPI+IRQ CD hotplug=%s LFN=%u max=%u codepage=%u\n",
        (UB *)MTFS_RA8P1_TEST_PROFILE_NAME, rounds,
        hotplug ? (UB *)"on" : (UB *)"off",
        FF_USE_LFN, FF_MAX_LFN, FF_CODE_PAGE);
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

    error = mtfs_ra_sd_spi_context_init(&sd_context, &config);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] context init FAIL mtfs=%d tk=%d fsp=%d\n",
            error, sd_context.last_kernel_error,
            sd_context.last_fsp_error);
        overall_failure = 1;
        goto test_done;
    }
    context_ready = 1;
    error = mtfs_ra8p1_card_detect_start(&media_context,
        &media_service, &sd_context, target_media_event, NULL);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] card detect start FAIL mtfs=%d tk=%d\n",
            error, media_service.last_kernel_error);
        overall_failure = 1;
        goto test_done;
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
        tm_printf((UB *)"[mtfs] CD initial raw=%u configured-active=%s irq=%u\n",
            cd.raw_level, cd.active_low ? (UB *)"low" : (UB *)"high",
            cd.irq_entries);
        if (hotplug) {
            target_print_card_detect_hardware();
        }
    }

    if (hotplug && !mtfs_media_is_present(&media_context)) {
        mtfs_block_status_t absent_status = 0U;
        mtfs_error_t absent_error = mtfs_block_status(device, &absent_status);
        if ((absent_error != MTFS_ERROR_NO_MEDIA) ||
            ((absent_status & MTFS_BLOCK_STATUS_MEDIA_PRESENT) != 0U)) {
            tm_printf((UB *)"[mtfs] initial ABSENT status FAIL: %d/0x%08x\n",
                absent_error, absent_status);
            overall_failure = 1;
            goto test_done;
        }
        tm_printf((UB *)"[mtfs] initial ABSENT status PASS: mtfs=%d flags=0x%08x\n",
            absent_error, absent_status);
        tm_printf((UB *)"[mtfs] ACTION REQUIRED: INSERT card now; waiting up to %u ms\n",
            MTFS_TARGET_HOTPLUG_WAIT_MS);
        if (!target_wait_media_event(MTFS_TARGET_MEDIA_INSERTED,
                "initial INSERTED")) {
            overall_failure = 1;
            goto test_done;
        }
    }

    error = mtfs_block_initialize(device);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] SD init FAIL mtfs=%d tk=%d fsp=%d r1=0x%02x\n",
            error, sd_context.last_kernel_error,
            sd_context.last_fsp_error, sd_context.last_r1);
        target_print_diagnostics(&sd_context);
        overall_failure = 1;
        goto test_done;
    }
    ++media_reinitialize_count;
    error = mtfs_block_registry_register(0U, device);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] pdrv 0 register FAIL: %d\n", error);
        overall_failure = 1;
        goto test_done;
    }
    registered = 1;

    for (round = 1U; round <= rounds; ++round) {
        mtfs_block_geometry_t geometry;
        mtfs_test_t test;
        int case_result;
        int round_failure = 0;

        tm_printf((UB *)"[mtfs] round %u/%u BEGIN\n",
            round, rounds);

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

        mtfs_test_begin(&test, "fatfs_lfn", target_reporter, NULL);
        case_result = test_fatfs_lfn(&test, "0:");
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

        if (hotplug) {
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
        mtfs_test_begin(&test, "fatfs_lfn_after_reinsert",
            target_reporter, NULL);
        case_result = test_fatfs_lfn(&test, "0:");
        if ((mtfs_test_finish(&test) != 0) || (case_result != 0)) {
            round_failure = 1;
        }
        }

round_done:
        tm_printf((UB *)"[mtfs] round %u/%u %s\n", round,
            rounds,
            round_failure ? (UB *)"FAIL" : (UB *)"PASS");
        if (round_failure) {
            overall_failure = 1;
        }
    }

test_done:
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
            overall_failure = 1;
        }
    }
    if (registered && (mtfs_block_registry_unregister(0U) != MTFS_OK)) {
        tm_printf((UB *)"[mtfs] registry cleanup FAIL\n");
        overall_failure = 1;
    }
    if (context_ready) {
        error = mtfs_ra_sd_spi_context_deinit(&sd_context);
        if (error != MTFS_OK) {
            tm_printf((UB *)"[mtfs] context cleanup FAIL: %d\n", error);
            overall_failure = 1;
        }
    }
    if (media_application_event_flag_id > 0) {
        (void)tk_del_flg(media_application_event_flag_id);
        media_application_event_flag_id = 0;
    }
    tm_printf((UB *)"[mtfs] storage command %s\n",
        overall_failure ? (UB *)"FAIL" : (UB *)"PASS");
    return overall_failure;
}

static void target_coordinator(INT start_code, void *opaque)
{
#if !MTFS_FF_FS_NORTC
    mtfs_error_t rtc_error;
    T_CMTX rtc_mutex = {
        .mtxatr = TA_INHERIT
    };
#endif

    (void)start_code;
    (void)opaque;
    if (mtfs_ra8p1_rsip_lock_init() != 0) {
        tm_printf((UB *)"[mtfs] RSIP mutex create FAIL\n");
        tk_exd_tsk();
    }
    mtfs_ra8p1_crypto_spike_banner();
#if !MTFS_FF_FS_NORTC
    rtc_mutex_id = tk_cre_mtx(&rtc_mutex);
    if (rtc_mutex_id <= 0) {
        tm_printf((UB *)"[mtfs] RTC mutex create FAIL: %d\n", rtc_mutex_id);
        mtfs_ra8p1_rsip_lock_deinit();
        tk_exd_tsk();
    }
    rtc_error = mtfs_ra_rtc_init(&rtc_context, &g_rtc0,
        target_rtc_lock, target_rtc_unlock, &rtc_mutex_id);
    if (rtc_error == MTFS_OK) {
        rtc_error = mtfs_time_provider_register(
            mtfs_ra_rtc_provider(&rtc_context));
    }
    if (rtc_error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] RTC provider init FAIL: %d fsp=%d vbt=0x%02x\n",
            rtc_error, rtc_context.last_fsp_error,
            rtc_context.vbatt_status_at_init);
    } else {
        mtfs_time_status_t rtc_status;
        (void)mtfs_time_get_status(&rtc_status);
        tm_printf((UB *)"[mtfs] RTC provider state=%u source=SUBCLK local-time vbt=0x%02x cold=%u source-init=%u\n",
            (UW)rtc_status, rtc_context.vbatt_status_at_init,
            (UW)rtc_context.backup_power_loss_at_init,
            (UW)rtc_context.clock_source_initialized);
    }
#endif
#if MTFS_TARGET_COMMAND_CONSOLE_ACTIVE
    tm_printf((UB *)"[mtfs] command console ready\n");
    target_command_console();
#else
    tm_printf((UB *)"[mtfs] command console disabled; coordinator stopped\n");
#endif
    mtfs_ra8p1_rsip_lock_deinit();
    tk_exd_tsk();
}

EXPORT INT usermain(void)
{
    T_CTSK coordinator = {
        .tskatr = TA_HLNG | TA_RNG3 | TA_USERBUF,
        .task = target_coordinator,
        .itskpri = 9,
        .stksz = sizeof(coordinator_stack.stack),
        .bufptr = coordinator_stack.stack
    };
    ID task_id;

    memset(coordinator_stack.guard, MTFS_TARGET_STACK_FILL,
        sizeof(coordinator_stack.guard));
    memset(coordinator_stack.stack, MTFS_TARGET_STACK_FILL,
        sizeof(coordinator_stack.stack));
    task_id = tk_cre_tsk(&coordinator);

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
    return 0;
}
