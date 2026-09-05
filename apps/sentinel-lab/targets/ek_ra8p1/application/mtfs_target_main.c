#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <mtkernel/lib/libtm/libtm.h>

#include "ff.h"
#include "mtfs_block_registry.h"
#include "mtfs_media.h"
#include "mtfs_media_service.h"
#include "mtfs_ra8p1_platform.h"
#include "mtfs_ra8p1_tflm_spike.h"
#include "mtfs_ra_sd_spi.h"
#include "mtfs_sentinel.h"
#include "mtfs_sentinel_lab_console.h"
#include "mtfs_sentinel_lab_run.h"
#include "mtfs_sentinel_recorder.h"

#define LAB_TARGET_ID (UINT32_C(0x52413850))
#define LAB_TRANSPORT_ID (UINT32_C(0x53504920))
#define LAB_RA_LIGHT_DELAY_US (1000U)
#define LAB_RA_MEDIUM_DELAY_US (4000U)
#define LAB_RA_STRONG_DELAY_US (12000U)
#if !defined(MTFS_SENTINEL_LAB_BUILD_RELEASE)
#error "MTFS_SENTINEL_LAB_BUILD_RELEASE must be defined by the build configuration"
#elif MTFS_SENTINEL_LAB_BUILD_RELEASE == 1
#define LAB_BUILD_TYPE "Release"
#elif MTFS_SENTINEL_LAB_BUILD_RELEASE == 0
#define LAB_BUILD_TYPE "Debug"
#else
#error "MTFS_SENTINEL_LAB_BUILD_RELEASE must be 0 or 1"
#endif

static mtfs_ra_sd_spi_context_t sd_context;
static mtfs_media_context_t media_context;
static mtfs_media_service_context_t media_service;
static FATFS tflm_spike_filesystem;
static volatile uint32_t tflm_heartbeat;
static volatile uint8_t tflm_heartbeat_stop;
#define LAB_STACK_SIZE (12U * 1024U)
#define HEARTBEAT_STACK_SIZE (2048U)
#define STACK_PATTERN (0xa5U)
static uint8_t lab_task_stack[LAB_STACK_SIZE] __attribute__((aligned(8)));
static uint8_t heartbeat_task_stack[HEARTBEAT_STACK_SIZE]
    __attribute__((aligned(8)));
#if MTFS_ENABLE_STORAGE_SENTINEL
static ID observer_mutex_id;
static mtfs_sentinel_lab_runtime_t lab_runtime;
#endif

static void lab_console_write(void *context, const char *text);

static uint32_t stack_high_water(const uint8_t *stack, uint32_t size)
{
    uint32_t untouched = 0U;
    while (untouched < size && stack[untouched] == STACK_PATTERN) {
        ++untouched;
    }
    return size - untouched;
}

static void tflm_heartbeat_task(INT start_code, void *context)
{
    (void)start_code;
    (void)context;
    while (tflm_heartbeat_stop == 0U) {
        ++tflm_heartbeat;
        (void)tk_dly_tsk(1U);
    }
    tk_ext_tsk();
}

#if MTFS_ENABLE_STORAGE_SENTINEL
static int sentinel_runtime_self_test(void)
{
    mtfs_sentinel_operation_feature_t operation;
    (void)memset(&operation, 0, sizeof(operation));
    operation.timing_samples = UINT64_C(1);
    operation.total_latency_us = UINT64_C(513);
    operation.average_latency_us = UINT64_C(513);
    operation.latency_histogram[10] = UINT64_C(1);
    if (mtfs_sentinel_histogram_bucket(UINT64_C(512)) != 9U ||
        mtfs_sentinel_histogram_bucket(UINT64_C(513)) != 10U ||
        mtfs_sentinel_histogram_bucket(UINT64_C(651)) != 10U ||
        mtfs_sentinel_histogram_bucket(UINT64_C(9766)) != 14U ||
        !mtfs_sentinel_operation_timing_is_consistent(&operation)) {
        return 0;
    }
    operation.latency_histogram[10] = 0U;
    operation.latency_histogram[9] = UINT64_C(1);
    return !mtfs_sentinel_operation_timing_is_consistent(&operation);
}

static mtfs_error_t observer_lock(void *opaque)
{
    return tk_loc_mtx(*(ID *)opaque, TMO_FEVR) >= E_OK ?
        MTFS_OK : MTFS_ERROR_NOT_READY;
}

static void observer_unlock(void *opaque)
{
    (void)tk_unl_mtx(*(ID *)opaque);
}

static mtfs_error_t transport_sample(void *opaque,
    mtfs_sentinel_transport_snapshot_t *snapshot)
{
    mtfs_ra_sd_spi_diagnostics_t diagnostics;
    mtfs_error_t error = mtfs_ra_sd_spi_diagnostics_get(opaque, &diagnostics);
    if (error != MTFS_OK) {
        return error;
    }
    snapshot->reset_epoch = diagnostics.reset_epoch;
    snapshot->validity_mask =
        MTFS_SENTINEL_TRANSPORT_VALID_TRANSPORT_ERRORS |
        MTFS_SENTINEL_TRANSPORT_VALID_TRANSFER_TIMEOUTS |
        MTFS_SENTINEL_TRANSPORT_VALID_READY_TIMEOUTS |
        MTFS_SENTINEL_TRANSPORT_VALID_CLOCK_ERRORS;
    snapshot->flags = MTFS_SENTINEL_TRANSPORT_FLAG_COUNTERS_SATURATE;
    snapshot->transport_errors = diagnostics.transfer_errors;
    snapshot->transfer_timeouts = diagnostics.token_timeouts;
    snapshot->ready_timeouts = diagnostics.ready_timeouts;
    snapshot->clock_errors = diagnostics.monotonic_clock_errors;
    if (diagnostics.transfer_errors == UINT32_MAX ||
        diagnostics.token_timeouts == UINT32_MAX ||
        diagnostics.ready_timeouts == UINT32_MAX ||
        diagnostics.monotonic_clock_errors == UINT32_MAX) {
        snapshot->flags |=
            MTFS_SENTINEL_TRANSPORT_FLAG_COUNTER_SATURATED;
    }
    return MTFS_OK;
}

static void collect_metadata(void *opaque,
    mtfs_sentinel_sample_metadata_t *metadata)
{
    mtfs_media_diagnostics_t diagnostics;
    (void)opaque;
    if (mtfs_media_diagnostics_get(&media_context, &diagnostics) == MTFS_OK) {
        metadata->media_generation = diagnostics.media_generation;
        metadata->media_reset_epoch = diagnostics.reset_epoch;
        metadata->inserted_events = diagnostics.inserted_events;
        metadata->removed_events = diagnostics.removed_events;
        metadata->error_events = diagnostics.error_events;
    }
}
#endif

#if MTFS_ENABLE_STORAGE_SENTINEL
static mtfs_error_t platform_prepare(void *opaque,
    mtfs_block_device_t **device)
{
    mtfs_ra_sd_spi_config_t sd_config;
    T_CMTX mutex = {.mtxatr = TA_INHERIT};
    (void)opaque;
    observer_mutex_id = tk_cre_mtx(&mutex);
    if (observer_mutex_id <= 0) return MTFS_ERROR_NOT_READY;
    mtfs_ra8p1_sd_spi_config(&sd_config);
    if (mtfs_ra_sd_spi_context_init(&sd_context, &sd_config) != MTFS_OK) {
        (void)tk_del_mtx(observer_mutex_id);
        observer_mutex_id = 0;
        return MTFS_ERROR_IO;
    }
    if (mtfs_ra8p1_card_detect_start(&media_context, &media_service,
            &sd_context, NULL, NULL) != MTFS_OK) {
        (void)mtfs_ra_sd_spi_context_deinit(&sd_context);
        (void)tk_del_mtx(observer_mutex_id);
        observer_mutex_id = 0;
        return MTFS_ERROR_IO;
    }
    *device = mtfs_ra_sd_spi_block_device(&sd_context);
    return MTFS_OK;
}

static void platform_finish(void *opaque)
{
    (void)opaque;
    (void)mtfs_ra8p1_card_detect_stop();
    (void)mtfs_ra_sd_spi_context_deinit(&sd_context);
    if (observer_mutex_id > 0) (void)tk_del_mtx(observer_mutex_id);
    observer_mutex_id = 0;
}

static void platform_sleep(void *opaque, uint32_t delay_ms)
{
    (void)opaque;
    (void)tk_dly_tsk(delay_ms);
}

static void platform_write(void *opaque, const char *text)
{
    (void)opaque;
    lab_console_write(NULL, text);
}

static int run_collection(mtfs_sentinel_lab_mode_t mode, uint32_t samples,
    uint32_t seed)
{
    mtfs_sentinel_lab_run_config_t config;
    (void)memset(&config, 0, sizeof(config));
    config.prepare = platform_prepare;
    config.finish = platform_finish;
    config.clock_us = mtfs_ra8p1_benchmark_clock_us;
    config.sentinel_clock = mtfs_ra8p1_sentinel_clock_us;
    config.sleep = platform_sleep;
    config.write = platform_write;
    config.collect_metadata = collect_metadata;
    config.observer_lock = observer_lock;
    config.observer_unlock = observer_unlock;
    config.observer_lock_context = &observer_mutex_id;
    config.transport_sample = transport_sample;
    config.transport_context = &sd_context;
    config.build_type = LAB_BUILD_TYPE;
    config.target_id = LAB_TARGET_ID;
    config.transport_id = LAB_TRANSPORT_ID;
    config.light_delay_us = LAB_RA_LIGHT_DELAY_US;
    config.medium_delay_us = LAB_RA_MEDIUM_DELAY_US;
    config.strong_delay_us = LAB_RA_STRONG_DELAY_US;
    return mtfs_sentinel_lab_run(&lab_runtime, &config, mode, samples, seed);
}

static int run_tflm_spike(uint32_t iterations)
{
    mtfs_block_device_t *device = NULL;
    T_CTSK heartbeat_task;
    ID heartbeat_id = 0;
    uint32_t heartbeat_before = 0U;
    uint32_t lab_used;
    uint32_t heartbeat_used;
    int cleanup_ok = 1;
    int fairness_ok;
    int stack_ok;
    int prepared = 0;
    int registered = 0;
    int mounted = 0;
    int result = -1;

    (void)memset(&heartbeat_task, 0, sizeof(heartbeat_task));
    (void)memset(heartbeat_task_stack, STACK_PATTERN,
        sizeof(heartbeat_task_stack));
    tflm_heartbeat = 0U;
    tflm_heartbeat_stop = 0U;
    heartbeat_task.tskatr = TA_HLNG | TA_RNG3 | TA_USERBUF;
    heartbeat_task.task = tflm_heartbeat_task;
    heartbeat_task.itskpri = 8;
    heartbeat_task.stksz = sizeof(heartbeat_task_stack);
    heartbeat_task.bufptr = heartbeat_task_stack;
    heartbeat_id = tk_cre_tsk(&heartbeat_task);
    if (heartbeat_id <= 0 || tk_sta_tsk(heartbeat_id, 0) < E_OK) {
        tm_printf((UB *)"[RA0.1] heartbeat-start=FAIL\n");
        goto cleanup;
    }
    (void)tk_dly_tsk(2U);
    heartbeat_before = tflm_heartbeat;

    if (platform_prepare(NULL, &device) != MTFS_OK) goto cleanup;
    prepared = 1;
    if (!mtfs_block_device_is_valid(device) ||
        mtfs_block_initialize(device) != MTFS_OK ||
        mtfs_block_registry_register(0U, device) != MTFS_OK) goto cleanup;
    registered = 1;
    if (f_mount(&tflm_spike_filesystem, "0:", 1U) != FR_OK) goto cleanup;
    mounted = 1;
    result = mtfs_ra8p1_tflm_spike_run(iterations, heartbeat_before,
        &tflm_heartbeat);

cleanup:
    if (mounted && f_mount(NULL, "0:", 0U) != FR_OK) cleanup_ok = 0;
    if (registered && mtfs_block_registry_unregister(0U) != MTFS_OK) {
        cleanup_ok = 0;
    }
    if (prepared) platform_finish(NULL);
    tflm_heartbeat_stop = 1U;
    if (heartbeat_id > 0) {
        (void)tk_dly_tsk(2U);
        if (tk_del_tsk(heartbeat_id) < E_OK) {
            (void)tk_ter_tsk(heartbeat_id);
            if (tk_del_tsk(heartbeat_id) < E_OK) cleanup_ok = 0;
        }
    }
    lab_used = stack_high_water(lab_task_stack, sizeof(lab_task_stack));
    heartbeat_used = stack_high_water(heartbeat_task_stack,
        sizeof(heartbeat_task_stack));
    fairness_ok = tflm_heartbeat > heartbeat_before;
    stack_ok = lab_used + 256U < sizeof(lab_task_stack) &&
        heartbeat_used + 256U < sizeof(heartbeat_task_stack);
    tm_printf((UB *)"[RA0.1] scheduler heartbeat-delta=%u fairness=%s\n",
        tflm_heartbeat - heartbeat_before,
        fairness_ok ? (UB *)"PASS" : (UB *)"FAIL");
    tm_printf((UB *)"[RA0.1] stack lab-used=%u/%u lab-margin=%u "
        "heartbeat-used=%u/%u heartbeat-margin=%u guard=%s\n",
        lab_used, (unsigned int)sizeof(lab_task_stack),
        (unsigned int)sizeof(lab_task_stack) - lab_used,
        heartbeat_used, (unsigned int)sizeof(heartbeat_task_stack),
        (unsigned int)sizeof(heartbeat_task_stack) - heartbeat_used,
        stack_ok ? (UB *)"PASS" : (UB *)"FAIL");
    if (!cleanup_ok || !fairness_ok || !stack_ok) result = -1;
    tm_printf((UB *)"[RA0.1] runtime-load exit=%d cleanup=%s\n", result,
        cleanup_ok ? (UB *)"PASS" : (UB *)"FAIL");
    return result;
}
#endif

#if MTFS_ENABLE_STORAGE_SENTINEL
static int parse_command(const char *line, const char *expected,
    uint32_t default_first, uint32_t default_second, uint32_t *first,
    uint32_t *second)
{
    char command[40];
    char extra;
    unsigned int parsed_first = 0U, parsed_second = 0U;
    int fields = sscanf(line, "%39s %u %u %c", command, &parsed_first,
        &parsed_second, &extra);
    if (fields < 1 || fields > 3 || strcmp(command, expected) != 0)
        return 0;
    *first = fields >= 2 ? (uint32_t)parsed_first : default_first;
    *second = fields >= 3 ? (uint32_t)parsed_second : default_second;
    return 1;
}
#endif

static void lab_console_write(void *context, const char *text)
{
    INT length = 0;
    (void)context;
    if (text == NULL) {
        return;
    }
    while (text[length] != '\0') {
        ++length;
    }
    if (length != 0) {
        tm_snd_dat((const UB *)text, length);
    }
}

static int lab_command(void *context, const char *line)
{
#if MTFS_ENABLE_STORAGE_SENTINEL
    uint32_t samples, seed;
#endif
    (void)context;
    if (strcmp(line, "help") == 0) {
#if MTFS_ENABLE_STORAGE_SENTINEL
        lab_console_write(NULL,
            "help\r\n"
            "record [samples]\r\n"
            "pseudo-collect-delay-ramp [samples-per-stage] [seed]\r\n"
            "pseudo-collect-hard-fault [samples] [seed]\r\n"
            "sentinel-tflm-spike [iterations]  load SRA_A.TFL/B from SD and run Ethos-U\r\n");
#else
        lab_console_write(NULL,
            "help    show this help\r\n"
            "record  measure workload continuously until board reset\r\n");
#endif
        return 1;
    }
#if MTFS_ENABLE_STORAGE_SENTINEL
    if (parse_command(line, "record", 0U, 0U, &samples, &seed) &&
        seed == 0U) {
        tm_printf((UB *)"# record samples=%u scenario_origin=natural\n",
            samples);
        tm_printf((UB *)"# record_exit=%d\n",
            run_collection(MTFS_SENTINEL_LAB_MODE_RECORD, samples, 0U));
        return 1;
    }
    if (parse_command(line, "pseudo-collect-delay-ramp", 100U, 1U,
            &samples, &seed) && samples != 0U && seed != 0U) {
        tm_printf((UB *)"# delay-ramp samples_per_stage=%u seed=%u\n",
            samples, seed);
        tm_printf((UB *)"# delay_ramp_exit=%d\n",
            run_collection(MTFS_SENTINEL_LAB_MODE_DELAY_RAMP, samples,
                seed));
        return 1;
    }
    if (parse_command(line, "pseudo-collect-hard-fault", 100U, 1U,
            &samples, &seed) && samples != 0U && seed != 0U) {
        tm_printf((UB *)"# hard-fault samples=%u seed=%u\n",
            samples, seed);
        tm_printf((UB *)"# hard_fault_exit=%d\n",
            run_collection(MTFS_SENTINEL_LAB_MODE_HARD_FAULT, samples,
                seed));
        return 1;
    }
    if (parse_command(line, "sentinel-tflm-spike", 100U, 0U,
            &samples, &seed) && samples != 0U && seed == 0U) {
        tm_printf((UB *)"# sentinel-tflm-spike iterations=%u\n", samples);
        tm_printf((UB *)"# sentinel_tflm_spike_exit=%d\n",
            run_tflm_spike(samples));
        return 1;
    }
#endif
    return 0;
}

static void lab_task(INT start_code, void *context)
{
    mtfs_sentinel_lab_console_t console;
    (void)start_code;
    (void)context;
    mtfs_sentinel_lab_console_init(&console, lab_console_write, NULL,
        lab_command, NULL);
    tm_printf((UB *)"\nmicroT-FS Storage Sentinel Lab\n");
    tm_printf((UB *)"# target: EK-RA8P1\n");
    tm_printf((UB *)"# transport: SPI\n");
    tm_printf((UB *)"# build: %s\n", (UB *)LAB_BUILD_TYPE);
#if MTFS_ENABLE_STORAGE_SENTINEL
    tm_printf((UB *)"# feature schema: v%u\n", MTFS_SENTINEL_SCHEMA_VERSION);
    tm_printf((UB *)"# arithmetic: portable-u64-v3\n");
    tm_printf((UB *)"# sentinel self-test: %s\n",
        (UB *)(sentinel_runtime_self_test() ? "PASS" : "FAIL"));
#else
    tm_printf((UB *)"# sentinel: disabled performance baseline\n");
#endif
    tm_printf((UB *)"# sample interval: 1000 ms\n");
    tm_printf((UB *)"# record writes and removes temporary files continuously\n");
    tm_printf((UB *)"# insert a FAT-formatted SD card before recording\n");
    lab_console_write(NULL, "Type help for commands.\r\n> ");
    for (;;) {
        mtfs_sentinel_lab_console_feed(&console, (char)tm_getchar(1));
    }
}

EXPORT INT usermain(void)
{
    (void)memset(lab_task_stack, STACK_PATTERN, sizeof(lab_task_stack));
    T_CTSK task = {
        .tskatr = TA_HLNG | TA_RNG3 | TA_USERBUF,
        .task = lab_task,
        .itskpri = 9,
        .stksz = sizeof(lab_task_stack),
        .bufptr = lab_task_stack
    };
    ID task_id = tk_cre_tsk(&task);
    if ((task_id <= 0) || (tk_sta_tsk(task_id, 0) < E_OK)) {
        tm_printf((UB *)"# sentinel-lab task start failed\n");
    }
    for (;;) {
        (void)tk_slp_tsk(TMO_FEVR);
    }
    return 0;
}
