#include <stddef.h>
#include <string.h>

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "stm32n6xx.h"

#include "mtfs_block_device.h"
#include "mtfs_block_diagnostics.h"
#include "mtfs_block_registry.h"
#include "ff.h"
#include "mtfs_stm32_sdmmc.h"
#include "mtfs_test.h"
#include "mtfs_benchmark.h"
#include "test_fatfs_lfn.h"
#include "test_fatfs_roundtrip.h"
#include "mtfs_stm32n6570_dk_platform.h"
#include "mtfs_target_concurrent.h"
#include "mtfs_stm32n6570_crypto_spike.h"
#include "mtfs_stm32n6570_crypto_work.h"
#include "mtfs_stm32n6570_model_test.h"
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
#include "mtfs_console.h"
#include "mtfs_console_tmonitor.h"
#include "test_fatfs_timestamp.h"
#define MTFS_TARGET_COMMAND_CONSOLE_ACTIVE (1)
#else
#define MTFS_TARGET_COMMAND_CONSOLE_ACTIVE (0)
#endif

#define MTFS_TEST_PROFILE_SMOKE   (1)
#define MTFS_TEST_PROFILE_NORMAL  (2)
#define MTFS_TEST_PROFILE_STRESS  (3)

#ifndef MTFS_STM32N6570_TEST_PROFILE
#define MTFS_STM32N6570_TEST_PROFILE MTFS_TEST_PROFILE_NORMAL
#endif

#define MTFS_TARGET_MEDIA_INSERTED (UINT32_C(1) << 0)
#define MTFS_TARGET_MEDIA_REMOVED  (UINT32_C(1) << 1)
#define MTFS_TARGET_MEDIA_ERROR    (UINT32_C(1) << 2)
#define MTFS_TARGET_HOTPLUG_WAIT_MS (120000)
#define MTFS_TARGET_TEST_ROUNDS_MAX (1000U)
#define MTFS_TARGET_COORDINATOR_STACK_SIZE (16U * 1024U)
#define MTFS_TARGET_STACK_GUARD_SIZE       (32U)
#define MTFS_TARGET_STACK_FILL             (0xA5U)
#define MTFS_TARGET_STACK_PASS_FREE        (4U * 1024U)
#define MTFS_TARGET_STACK_MIN_FREE         (2U * 1024U)

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
typedef struct target_coordinator_stack
{
    UB guard[MTFS_TARGET_STACK_GUARD_SIZE];
    UW stack[MTFS_TARGET_COORDINATOR_STACK_SIZE / sizeof(UW)];
} target_coordinator_stack_t;

static target_coordinator_stack_t coordinator_stack
    __attribute__((aligned(8)));
static uint8_t sector_zero_multi[MTFS_STM32_SDMMC_SECTOR_SIZE * 2U];
#if MTFS_TARGET_COMMAND_CONSOLE_ACTIVE
static uint8_t benchmark_buffer[MTFS_BENCHMARK_BUFFER_BYTES];
#endif
static void target_media_event(void *opaque, mtfs_media_event_t event,
    mtfs_media_state_t state);
static void target_print_diagnostics(
    mtfs_stm32_sdmmc_context_t *context);
static int target_run_storage_test(unsigned int rounds, int hotplug);
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

#if MTFS_TARGET_COMMAND_CONSOLE_ACTIVE
static int target_console_command(void *opaque, const char *line);

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
    size_t index;

    for (index = 0U; index < sizeof(coordinator_stack.guard); ++index) {
        if (bytes[index] != (UB)MTFS_TARGET_STACK_FILL) {
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
    unsigned int index;
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

    for (index = 0U; index < worker_count; ++index) {
        char name[] = "worker-0";
        const char *status;

        name[7] = (char)('0' + index);
        if (!mtfs_target_concurrent_stack_watermark(index, &watermark)) {
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

static int target_crypto_command_uses_storage(const char *line)
{
    return (strcmp(line, "crypto-negative") == 0) ||
        (strcmp(line, "crypto-package-test") == 0) ||
        (strcmp(line, "model-info") == 0) ||
        (strcmp(line, "model-load") == 0) ||
        (strcmp(line, "model-negative") == 0) ||
        (strcmp(line, "model-hotplug") == 0);
}

static void target_run_crypto_storage_command(const char *line)
{
    mtfs_stm32_sdmmc_config_t config;
    mtfs_block_device_t *device = NULL;
    mtfs_error_t error = MTFS_OK;
    const char *stage = "context-init";
    int registered = 0;
    int context_ready = 0;
    int media_ready = 0;
    int setup_failed = 1;

    mtfs_stm32n6570_dk_sdmmc_config(&config);
    error = mtfs_stm32_sdmmc_context_init(&sd_context, &config);
    if (error != MTFS_OK) goto cleanup;
    context_ready = 1;

    stage = "card-detect";
    error = mtfs_stm32n6570_dk_card_detect_start(&media_context,
        &media_service, &sd_context, target_media_event, NULL);
    if (error != MTFS_OK) goto cleanup;
    media_ready = 1;

    stage = "initialize";
    device = mtfs_stm32_sdmmc_block_device(&sd_context);
    error = mtfs_block_initialize(device);
    if (error != MTFS_OK) goto cleanup;

    stage = "registry";
    error = mtfs_block_registry_register(0U, device);
    if (error != MTFS_OK) goto cleanup;
    registered = 1;
    setup_failed = 0;
    if (!mtfs_stm32n6570_model_command(line, &media_context)) {
        (void)mtfs_stm32n6570_crypto_command(line);
    }

cleanup:
    if (setup_failed) {
        tm_printf((UB *)"[crypto] storage setup FAIL stage=%s mtfs=%d\n",
            (UB *)stage, error);
    }
    if (media_ready &&
        (mtfs_stm32n6570_dk_card_detect_stop() != MTFS_OK)) {
        tm_printf((UB *)"[crypto] storage cleanup FAIL stage=card-detect\n");
    }
    if (registered && (mtfs_block_registry_unregister(0U) != MTFS_OK)) {
        tm_printf((UB *)"[crypto] storage cleanup FAIL stage=registry\n");
    }
    if (context_ready &&
        (mtfs_stm32_sdmmc_context_deinit(&sd_context) != MTFS_OK)) {
        tm_printf((UB *)"[crypto] storage cleanup FAIL stage=context\n");
    }
}

static void target_benchmark_log(void *opaque, const char *line)
{
    (void)opaque;
    tm_printf((UB *)"%s\n", (UB *)line);
}

static void target_benchmark_info(void *opaque,
    mtfs_benchmark_log_fn log, void *log_context)
{
    const mtfs_stm32_sdmmc_context_t *context =
        (const mtfs_stm32_sdmmc_context_t *)opaque;
    const SD_HandleTypeDef *hal_sd = context->config.hal_sd;
    uint32_t clkcr = hal_sd->Instance->CLKCR;
    uint32_t bus_width = 1U;
    (void)log;
    (void)log_context;

    if ((clkcr & SDMMC_CLKCR_WIDBUS) == SDMMC_BUS_WIDE_4B) {
        bus_width = 4U;
    } else if ((clkcr & SDMMC_CLKCR_WIDBUS) == SDMMC_BUS_WIDE_8B) {
        bus_width = 8U;
    }
    tm_printf((UB *)"[BENCH] target card=%s transport=SDMMC2 bus_width=%u clock_hz=%u mode=%s\n",
        hal_sd->SdCard.CardType == CARD_SDHC_SDXC
            ? (UB *)"SDHC/SDXC" : (UB *)"SDSC",
        bus_width, mtfs_stm32n6570_dk_sdmmc_clock_hz(),
        context->config.use_idma ? (UB *)"idma_irq" : (UB *)"polling");
    tm_printf((UB *)"[BENCH] target block_mapping=%s multi_request=%s cache_i=%s cache_d=%s\n",
        context->config.use_idma
            ? (UB *)"HAL_SD_ReadBlocks_DMA/HAL_SD_WriteBlocks_DMA"
            : (UB *)"HAL_SD_ReadBlocks/HAL_SD_WriteBlocks-per-sector",
        context->config.use_idma ? (UB *)"preserved" : (UB *)"split",
        (SCB->CCR & SCB_CCR_IC_Msk) ? (UB *)"enabled" : (UB *)"disabled",
        (SCB->CCR & SCB_CCR_DC_Msk) ? (UB *)"enabled" : (UB *)"disabled");
    tm_printf((UB *)"[BENCH] clock source=microtkernel_uptime+systick resolution=core_cycle tick_ms=10 monotonic=yes\n");
}

static int target_run_benchmark(
    mtfs_benchmark_profile_t profile, int info_only)
{
    mtfs_stm32_sdmmc_config_t sd_config;
    mtfs_benchmark_config_t benchmark;
    mtfs_block_device_t *device = NULL;
    mtfs_error_t error;
    int failure = 1;
    int registered = 0;
    int context_ready = 0;
    int media_ready = 0;

    mtfs_stm32n6570_dk_sdmmc_config(&sd_config);
    error = mtfs_stm32_sdmmc_context_init(&sd_context, &sd_config);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[BENCH] setup stage=context_init error=%d\n", error);
        goto cleanup;
    }
    context_ready = 1;
    error = mtfs_stm32n6570_dk_card_detect_start(&media_context,
        &media_service, &sd_context, target_media_event, NULL);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[BENCH] setup stage=card_detect error=%d\n", error);
        goto cleanup;
    }
    media_ready = 1;
    device = mtfs_stm32_sdmmc_block_device(&sd_context);
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
    benchmark.board_name = "STM32N6570-DK";
#ifdef __OPTIMIZE__
    benchmark.build_name = "optimized";
#else
    benchmark.build_name = "debug";
#endif
    benchmark.clock_us = mtfs_stm32n6570_dk_benchmark_clock_us;
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
    if (media_ready &&
        (mtfs_stm32n6570_dk_card_detect_stop() != MTFS_OK)) {
        tm_printf((UB *)"[BENCH] cleanup stage=card_detect status=FAIL\n");
        failure = 1;
    }
    if (registered && (mtfs_block_registry_unregister(0U) != MTFS_OK)) {
        tm_printf((UB *)"[BENCH] cleanup stage=registry status=FAIL\n");
        failure = 1;
    }
    if (context_ready &&
        (mtfs_stm32_sdmmc_context_deinit(&sd_context) != MTFS_OK)) {
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
    mtfs_console_set_extension(&console, target_console_command, NULL,
        "test-roundtrip [rounds]   run storage suite (default: profile)\r\n"
        "test-hotplug              run one remove/reinsert storage test\r\n"
        "test-fatfs-time           verify FatFs timestamp against RTC\r\n"
        "bench-info                print ST benchmark conditions\r\n"
        "bench-smoke               run short non-destructive benchmark\r\n"
        "bench-normal              run 1 MiB baseline benchmark\r\n"
        "test-diagnostics-reset    verify reset on one active context\r\n"
        "diag                      print common/media/ST snapshots\r\n"
        "diag-reset                reset diagnostic counters only\r\n"
        "diag-help                 explain diagnostic commands\r\n"
        "stack-highwater           print coordinator/worker peak stack use\r\n"
        "crypto-info               print SAES/DHUK execution context\r\n"
        "crypto-consistency        run AES-256-GCM size/reinit tests\r\n"
        "crypto-negative           reject tag/ciphertext corruption\r\n"
        "crypto-package-test       test provisioned sealed package\r\n"
        "model-info                authenticate and show trusted model info\r\n"
        "model-load                load and verify MTFSTEST.MTF via model API\r\n"
        "model-negative            reject package/policy mutations via model API\r\n"
        "model-hotplug             verify removal cleanup and reinsertion recovery\r\n");
    mtfs_console_tmonitor_write(NULL,
        "microT-FS STM32N6570-DK command console\r\n"
        "Commands: RTC, storage, and STM32 SAES/DHUK crypto spike.\r\n"
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
    mtfs_stm32n6570_dk_card_detect_diagnostics_t cd;
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
    mtfs_stm32n6570_dk_get_card_detect_diagnostics(&cd);
    (void)mtfs_media_diagnostics_get(&media_context, &media_diagnostics);
    tm_printf((UB *)"[mtfs] CD raw=%u active=%s irq=%u rise=%u fall=%u debounce=%u/%u events=%u/%u/%u\n",
        cd.raw_level, cd.active_low ? (UB *)"low" : (UB *)"high",
        cd.irq_entries, cd.rising_edges, cd.falling_edges,
        media_diagnostics.debounce_starts,
        media_diagnostics.debounce_rechecks,
        media_inserted_events, media_removed_events, media_error_events);
}

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

static int target_check_idma_diagnostics(
    mtfs_test_t *test, mtfs_stm32_sdmmc_context_t *context)
{
    mtfs_stm32_sdmmc_diagnostics_t snapshot;
    const mtfs_stm32_sdmmc_diagnostics_t *diagnostics = &snapshot;

    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_sdmmc_diagnostics_get(context, &snapshot) == MTFS_OK,
            "typed STM32 SDMMC diagnostics snapshot is available")) {
        return 1;
    }

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
    mtfs_stm32_sdmmc_context_t *context)
{
    mtfs_stm32_sdmmc_diagnostics_t port_snapshot;
    mtfs_block_diagnostics_t common;
    mtfs_media_diagnostics_t media;
    const mtfs_stm32_sdmmc_diagnostics_t *diagnostics = &port_snapshot;
    if (mtfs_stm32_sdmmc_diagnostics_get(context, &port_snapshot) != MTFS_OK) {
        tm_printf((UB *)"[diag] STM32 SDMMC snapshot unavailable\n");
        return;
    }
    if (mtfs_block_diagnostics_get(
            mtfs_stm32_sdmmc_block_device(context), &common) == MTFS_OK) {
        tm_printf((UB *)"[diag] common v=%u size=%u epoch=%u op=%u error=%d status=0x%08x geometry=%u/0x%08x%08x/%u\n",
            common.api_version, common.struct_size, common.reset_epoch,
            common.last_operation, common.last_error, common.status,
            common.sector_size, (UW)(common.sector_count >> 32U),
            (UW)common.sector_count, common.erase_block_size);
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
    tm_printf((UB *)"[diag] st v=%u size=%u epoch=%u validity=0x%08x mode=%s state=%u/%u/%u geometry=%u/0x%08x%08x/%u bounce=%u line=%u aligned=%u\n",
        diagnostics->api_version,
        diagnostics->struct_size,
        diagnostics->reset_epoch,
        diagnostics->validity_mask,
        diagnostics->use_idma ? (UB *)"IDMA+IRQ" : (UB *)"polling",
        diagnostics->initialized,
        diagnostics->hal_initialized,
        diagnostics->transfer_active,
        diagnostics->sector_size,
        (UW)(diagnostics->sector_count >> 32U),
        (UW)diagnostics->sector_count,
        diagnostics->erase_block_size,
        diagnostics->bounce_buffer_size,
        diagnostics->cache_line_size,
        ((uintptr_t)context->bounce_buffer &
            (diagnostics->cache_line_size - 1U)) == 0U);
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
    tm_printf((UB *)"[mtfs] last mtfs=%d tk=%d hal=%u/0x%08x transfer-hal=0x%08x clkcr(snapshot)=0x%08x hwfc=%u div=%u\n",
        diagnostics->last_error,
        diagnostics->last_kernel_error,
        diagnostics->last_hal_status,
        diagnostics->last_hal_error,
        diagnostics->transfer_hal_error,
        diagnostics->last_clkcr,
        (diagnostics->last_clkcr & SDMMC_CLKCR_HWFC_EN) != 0U,
        diagnostics->last_clkcr & SDMMC_CLKCR_CLKDIV);
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

static int target_st_counters_are_clear(
    const mtfs_stm32_sdmmc_diagnostics_t *diagnostics)
{
    return (diagnostics->irq_entries == 0U) &&
        (diagnostics->rx_complete_callbacks == 0U) &&
        (diagnostics->tx_complete_callbacks == 0U) &&
        (diagnostics->error_callbacks == 0U) &&
        (diagnostics->read_single_starts == 0U) &&
        (diagnostics->read_multi_starts == 0U) &&
        (diagnostics->write_single_starts == 0U) &&
        (diagnostics->write_multi_starts == 0U) &&
        (diagnostics->read_max_blocks == 0U) &&
        (diagnostics->write_max_blocks == 0U) &&
        (diagnostics->aborts == 0U) &&
        (diagnostics->media_removal_notifications == 0U) &&
        (diagnostics->media_wait_wakeups == 0U) &&
        (diagnostics->completion_timeouts == 0U) &&
        (diagnostics->card_state_timeouts == 0U);
}

static int target_run_diagnostics_reset_test(void)
{
    mtfs_stm32_sdmmc_config_t config;
    mtfs_block_device_t *device = NULL;
    mtfs_block_geometry_t geometry;
    mtfs_block_status_t status = 0U;
    mtfs_block_diagnostics_t common_before;
    mtfs_block_diagnostics_t common_reset;
    mtfs_block_diagnostics_t common_after;
    mtfs_media_diagnostics_t media_before;
    mtfs_media_diagnostics_t media_reset;
    mtfs_media_diagnostics_t media_after;
    mtfs_stm32_sdmmc_diagnostics_t port_before;
    mtfs_stm32_sdmmc_diagnostics_t port_reset;
    mtfs_stm32_sdmmc_diagnostics_t port_after;
    mtfs_test_t test;
    mtfs_error_t error;
    int registered = 0;
    int context_ready = 0;
    int media_ready = 0;
    int failure;

    mtfs_test_begin(&test, "diagnostics_reset_active_context",
        target_reporter, NULL);
    mtfs_stm32n6570_dk_sdmmc_config(&config);
    error = mtfs_stm32_sdmmc_context_init(&sd_context, &config);
    if (!MTFS_TEST_CHECK(&test, error == MTFS_OK,
            "ST context initialization succeeds")) goto cleanup;
    context_ready = 1;

    error = mtfs_stm32n6570_dk_card_detect_start(&media_context,
        &media_service, &sd_context, target_media_event, NULL);
    if (!MTFS_TEST_CHECK(&test, error == MTFS_OK,
            "media context starts")) goto cleanup;
    media_ready = 1;
    device = mtfs_stm32_sdmmc_block_device(&sd_context);
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
        (geometry.sector_size == MTFS_STM32_SDMMC_SECTOR_SIZE) &&
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
            mtfs_stm32_sdmmc_diagnostics_get(&sd_context, &port_before) ==
                MTFS_OK,
            "ST snapshot is available before reset")) goto cleanup;
    (void)MTFS_TEST_CHECK(&test,
        (common_before.read_calls > 0U) &&
            (common_before.read_sectors_completed > 0U) &&
            ((port_before.read_single_starts +
                port_before.read_multi_starts) > 0U),
        "I/O counters increase before reset");

    error = mtfs_block_diagnostics_reset(device);
    (void)MTFS_TEST_CHECK(&test, error == MTFS_OK,
        "common diagnostics reset succeeds");
    error = mtfs_media_diagnostics_reset(&media_context);
    (void)MTFS_TEST_CHECK(&test, error == MTFS_OK,
        "media diagnostics reset succeeds");
    error = mtfs_stm32_sdmmc_diagnostics_reset(&sd_context);
    (void)MTFS_TEST_CHECK(&test, error == MTFS_OK,
        "ST diagnostics reset succeeds");

    if (!MTFS_TEST_CHECK(&test,
            mtfs_block_diagnostics_get(device, &common_reset) == MTFS_OK,
            "common snapshot is available after reset")) goto cleanup;
    if (!MTFS_TEST_CHECK(&test,
            mtfs_media_diagnostics_get(&media_context, &media_reset) ==
                MTFS_OK,
            "media snapshot is available after reset")) goto cleanup;
    if (!MTFS_TEST_CHECK(&test,
            mtfs_stm32_sdmmc_diagnostics_get(&sd_context, &port_reset) ==
                MTFS_OK,
            "ST snapshot is available after reset")) goto cleanup;

    (void)MTFS_TEST_CHECK(&test,
        common_reset.reset_epoch == common_before.reset_epoch + 1U,
        "common reset epoch advances");
    (void)MTFS_TEST_CHECK(&test,
        media_reset.reset_epoch == media_before.reset_epoch + 1U,
        "media reset epoch advances");
    (void)MTFS_TEST_CHECK(&test,
        port_reset.reset_epoch == port_before.reset_epoch + 1U,
        "ST reset epoch advances");
    (void)MTFS_TEST_CHECK(&test,
        target_common_counters_are_clear(&common_reset),
        "all common counters clear");
    (void)MTFS_TEST_CHECK(&test,
        target_media_counters_are_clear(&media_reset),
        "all media counters clear");
    (void)MTFS_TEST_CHECK(&test,
        target_st_counters_are_clear(&port_reset),
        "all ST counters clear");
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
            (port_reset.last_clkcr == port_before.last_clkcr) &&
            (port_reset.last_hal_status == port_before.last_hal_status) &&
            (port_reset.last_hal_error == port_before.last_hal_error) &&
            (port_reset.transfer_hal_error ==
                port_before.transfer_hal_error) &&
            (port_reset.last_kernel_error == port_before.last_kernel_error) &&
            (port_reset.last_error == port_before.last_error) &&
            (port_reset.sector_count == port_before.sector_count) &&
            (port_reset.sector_size == port_before.sector_size) &&
            (port_reset.erase_block_size == port_before.erase_block_size) &&
            (port_reset.bounce_buffer_size ==
                port_before.bounce_buffer_size) &&
            (port_reset.cache_line_size == port_before.cache_line_size) &&
            (port_reset.use_idma == port_before.use_idma) &&
            (port_reset.initialized == port_before.initialized) &&
            (port_reset.hal_initialized == port_before.hal_initialized) &&
            (port_reset.transfer_active == port_before.transfer_active) &&
            (port_reset.initialized != 0U) &&
            (port_reset.hal_initialized != 0U) &&
            (port_reset.transfer_active == 0U),
        "ST cached and live state are preserved");

    error = mtfs_block_read(device, sector_zero_multi, 0U, 1U);
    (void)MTFS_TEST_CHECK(&test, error == MTFS_OK,
        "raw read succeeds on the same context after reset");
    (void)MTFS_TEST_CHECK(&test,
        memcmp(sector_zero_single, sector_zero_multi,
            MTFS_STM32_SDMMC_SECTOR_SIZE) == 0,
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
            mtfs_stm32_sdmmc_diagnostics_get(&sd_context, &port_after) ==
                MTFS_OK,
            "ST snapshot is available after post-reset I/O")) goto cleanup;
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
            (port_after.read_single_starts == 1U) &&
            (port_after.read_multi_starts == 0U) &&
            (port_after.read_max_blocks == 1U) &&
            (port_after.error_callbacks == 0U) &&
            (port_after.completion_timeouts == 0U) &&
            (port_after.card_state_timeouts == 0U) &&
            (port_after.use_idma
                ? ((port_after.irq_entries > 0U) &&
                    (port_after.rx_complete_callbacks == 1U))
                : ((port_after.irq_entries == 0U) &&
                    (port_after.rx_complete_callbacks == 0U))),
        "ST transfer counters restart from zero for the selected mode");

cleanup:
    if (context_ready) target_print_diagnostics(&sd_context);
    if (media_ready) {
        error = mtfs_stm32n6570_dk_card_detect_stop();
        (void)MTFS_TEST_CHECK(&test, error == MTFS_OK,
            "media context cleanup succeeds");
    }
    if (registered) {
        error = mtfs_block_registry_unregister(0U);
        (void)MTFS_TEST_CHECK(&test, error == MTFS_OK,
            "block registry cleanup succeeds");
    }
    if (context_ready) {
        error = mtfs_stm32_sdmmc_context_deinit(&sd_context);
        (void)MTFS_TEST_CHECK(&test, error == MTFS_OK,
            "ST context cleanup succeeds");
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

static int target_console_command(void *opaque, const char *line)
{
    static const char command[] = "test-roundtrip";
    unsigned int rounds = MTFS_STM32N6570_TEST_ROUNDS;
    size_t offset = sizeof(command) - 1U;
    int parse_result = 0;

    (void)opaque;
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
                MTFS_TARGET_TEST_ROUNDS_MAX, MTFS_STM32N6570_TEST_ROUNDS);
        } else {
            (void)target_run_storage_test(rounds, 0);
        }
        return 1;
    }
    if (strcmp(line, "test-hotplug") == 0) {
        (void)target_run_storage_test(1U, 1);
        return 1;
    }
    if (target_crypto_command_uses_storage(line)) {
        target_run_crypto_storage_command(line);
        return 1;
    }
    if (mtfs_stm32n6570_crypto_command(line)) {
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
            mtfs_stm32_sdmmc_block_device(&sd_context));
        mtfs_error_t media = mtfs_media_diagnostics_reset(&media_context);
        mtfs_error_t port = mtfs_stm32_sdmmc_diagnostics_reset(&sd_context);
        tm_printf((UB *)"[diag] reset common=%d media=%d st=%d (I/O/media state unchanged)\n",
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
    mtfs_stm32_sdmmc_config_t config;
    mtfs_stm32n6570_dk_rif_diagnostics_t rif;
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
    mtfs_stm32n6570_dk_sdmmc_config(&config);
    if (hotplug) {
        media_application_event_flag_id = tk_cre_flg(&media_flag_config);
        if (media_application_event_flag_id <= 0) {
            tm_printf((UB *)"[mtfs] application media event flag create FAIL: %d\n",
                media_application_event_flag_id);
            media_application_event_flag_id = 0;
            return 1;
        }
    }
    mtfs_stm32n6570_dk_get_rif_diagnostics(&rif);
    tm_printf((UB *)"\n[mtfs] STM32N6570-DK storage test: profile=%s rounds=%u path=%s hotplug=%s LFN=%u max=%u codepage=%u\n",
        (UB *)MTFS_STM32N6570_TEST_PROFILE_NAME,
        rounds,
        config.use_idma ? (UB *)"IDMA+IRQ" : (UB *)"polling fallback",
        hotplug ? (UB *)"on" : (UB *)"off",
        FF_USE_LFN, FF_MAX_LFN, FF_CODE_PAGE);
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

    error = mtfs_stm32_sdmmc_context_init(&sd_context, &config);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] context init FAIL mtfs=%d tk=%d hal=%u/0x%08x\n",
            error, sd_context.last_kernel_error,
            sd_context.last_hal_status, sd_context.last_hal_error);
        overall_failure = 1;
        goto test_done;
    }
    context_ready = 1;
    error = mtfs_stm32n6570_dk_card_detect_start(&media_context,
        &media_service, &sd_context, target_media_event, NULL);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] card detect start FAIL mtfs=%d tk=%d\n",
            error, media_service.last_kernel_error);
        overall_failure = 1;
        goto test_done;
    }
    media_ready = 1;
    device = mtfs_stm32_sdmmc_block_device(&sd_context);
    tm_printf((UB *)"[mtfs] context=%p bounce=%p size=%u align32=%u cd-debounce=%u ms\n",
        &sd_context, sd_context.bounce_buffer,
        (UW)sizeof(sd_context.bounce_buffer),
        ((uintptr_t)sd_context.bounce_buffer & 31U) == 0U,
        media_context.config.debounce_ms);

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
        tm_printf((UB *)"[mtfs] SD init FAIL mtfs=%d tk=%d hal=%u/0x%08x\n",
            error, sd_context.last_kernel_error,
            sd_context.last_hal_status, sd_context.last_hal_error);
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

        mtfs_test_begin(&test, "sdmmc_idma_diagnostics",
            target_reporter, NULL);
        case_result = target_check_idma_diagnostics(&test, &sd_context);
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
            overall_failure = 1;
        }
    }
    if (registered && (mtfs_block_registry_unregister(0U) != MTFS_OK)) {
        tm_printf((UB *)"[mtfs] registry cleanup FAIL\n");
        overall_failure = 1;
    }
    if (context_ready) {
        error = mtfs_stm32_sdmmc_context_deinit(&sd_context);
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
    if (mtfs_stm32n6570_saes_lock_init() != 0) {
        tm_printf((UB *)"[mtfs] global SAES mutex create FAIL\n");
        tk_exd_tsk();
    }
#if !MTFS_FF_FS_NORTC
    rtc_mutex_id = tk_cre_mtx(&rtc_mutex);
    if (rtc_mutex_id <= 0) {
        tm_printf((UB *)"[mtfs] RTC mutex create FAIL: %d\n", rtc_mutex_id);
        mtfs_stm32n6570_saes_lock_deinit();
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
    } else {
        mtfs_time_status_t rtc_status;
        (void)mtfs_time_get_status(&rtc_status);
        tm_printf((UB *)"[mtfs] RTC provider state=%u source=LSI local-time reset=0x%08x\n",
            (UW)rtc_status, rtc_context.reset_flags_at_init);
    }
#endif
#if MTFS_TARGET_COMMAND_CONSOLE_ACTIVE
    tm_printf((UB *)"[mtfs] command console ready\n");
    target_command_console();
#else
    tm_printf((UB *)"[mtfs] command console disabled; coordinator stopped\n");
#endif
    mtfs_stm32n6570_saes_lock_deinit();
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

    (void)memset(coordinator_stack.guard, MTFS_TARGET_STACK_FILL,
        sizeof(coordinator_stack.guard));
    (void)memset(coordinator_stack.stack, MTFS_TARGET_STACK_FILL,
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
}
