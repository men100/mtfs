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
#include "mtfs_ra8p1_sentinel_inference.h"
#include "mtfs_ra8p1_sentinel_runtime.h"
#include "mtfs_ra8p1_validation.h"
#include "mtfs_ra_sd_spi.h"
#include "mtfs_sentinel.h"
#include "mtfs_sentinel_lab_console.h"
#include "mtfs_sentinel_lab_run.h"
#include "mtfs_sentinel_monitor.h"
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
static FATFS validation_filesystem;
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

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE && MTFS_ENABLE_SEALED_MODEL
typedef struct target_monitor_context
{
    mtfs_sentinel_lab_run_config_t lab_config;
    mtfs_sentinel_lab_window_config_t window_config;
    mtfs_sentinel_monitor_t monitor;
    uint32_t marker;
    uint32_t seed;
    uint8_t pseudo;
    uint8_t mounted;
    uint8_t inject_npu_failure;
    uint8_t npu_failure_injected;
    uint8_t injection_active;
    uint8_t hotplug;
} target_monitor_context_t;

static int monitor_wait_media(int present)
{
    uint32_t count;
    for (count = 0U; count < 12000U; ++count) {
        if (!!mtfs_media_is_present(&media_context) == !!present) return 0;
        (void)tk_dly_tsk(10U);
    }
    return 1;
}

static void monitor_injected_delay(void *opaque, uint32_t delay_us)
{
    uint32_t delay_ms;
    (void)opaque;
    delay_ms = delay_us > UINT32_MAX - 999U ? UINT32_MAX / 1000U :
        (delay_us + 999U) / 1000U;
    platform_sleep(NULL, delay_ms);
}

static mtfs_error_t monitor_media_ready(void *opaque)
{
    target_monitor_context_t *context = opaque;
    mtfs_media_state_t state = mtfs_media_state(&media_context);
    if (state != MTFS_MEDIA_STATE_PRESENT) {
        if (context->mounted != 0U) {
            (void)f_mount(NULL, "0:", 0U);
            context->mounted = 0U;
        }
        return state == MTFS_MEDIA_STATE_ABSENT ? MTFS_ERROR_NO_MEDIA :
            MTFS_ERROR_NOT_READY;
    }
    if (context->mounted == 0U) {
        if (f_mount(&lab_runtime.filesystem, "0:", 1U) != FR_OK)
            return MTFS_ERROR_NOT_READY;
        context->mounted = 1U;
    }
    return MTFS_OK;
}

static mtfs_sentinel_monitor_media_state_t monitor_media_state(void)
{
    mtfs_media_state_t state = mtfs_media_state(&media_context);
    if (state == MTFS_MEDIA_STATE_PRESENT)
        return MTFS_SENTINEL_MONITOR_MEDIA_PRESENT;
    if (state == MTFS_MEDIA_STATE_ABSENT)
        return MTFS_SENTINEL_MONITOR_MEDIA_ABSENT;
    return MTFS_SENTINEL_MONITOR_MEDIA_NOT_READY;
}

static mtfs_error_t monitor_stage_begin(void *opaque, uint32_t stage)
{
    target_monitor_context_t *context = opaque;
    mtfs_sentinel_dataset_metadata_t metadata;
    mtfs_sentinel_lab_configure_stage(&context->lab_config, &lab_runtime,
        context->pseudo != 0U ? MTFS_SENTINEL_LAB_MODE_DELAY_RAMP :
            MTFS_SENTINEL_LAB_MODE_RECORD,
        stage, context->seed, &metadata);
    context->injection_active = context->pseudo != 0U && stage >= 1U &&
        stage <= 3U ? 1U : 0U;
    return MTFS_OK;
}

static mtfs_error_t monitor_stage_end(void *opaque, uint32_t stage)
{
    target_monitor_context_t *context = opaque;
    (void)stage;
    context->injection_active = 0U;
    mtfs_sentinel_lab_injector_disable(&lab_runtime.injector);
    return MTFS_OK;
}

static const char *monitor_stage_name(void *opaque, uint32_t stage)
{
    static const char *const names[] = {
        "baseline", "light", "medium", "strong", "recovery"
    };
    target_monitor_context_t *context = opaque;
    if (context->hotplug != 0U) return "hotplug-lifecycle";
    return context->pseudo != 0U && stage < 5U ? names[stage] : "natural";
}

static mtfs_error_t monitor_acquire(void *opaque, uint32_t stage,
    uint32_t index, mtfs_sentinel_monitor_input_t *input)
{
    target_monitor_context_t *context = opaque;
    (void)stage;
    if (context->hotplug != 0U && stage == 0U && index == 1U) {
        tm_printf((UB *)"[sentinel-monitor-hotplug] ACTION REQUIRED: REMOVE card; resident model retained\n");
        if (monitor_wait_media(0) != 0) return MTFS_ERROR_NOT_READY;
    } else if (context->hotplug != 0U && stage == 0U && index == 2U) {
        tm_printf((UB *)"[sentinel-monitor-hotplug] ACTION REQUIRED: REINSERT card\n");
        if (monitor_wait_media(1) != 0) return MTFS_ERROR_NOT_READY;
    }
    ++context->marker;
    (void)mtfs_sentinel_lab_window_step(&lab_runtime.window,
        &context->window_config, context->marker, NULL, NULL,
        &input->window);
    input->media_state = monitor_media_state();
    if (input->window.sample_status == MTFS_OK)
        lab_runtime.frame = input->window.feature;
    return MTFS_OK;
}

static mtfs_error_t monitor_provider_open(void *opaque,
    uint64_t *threshold_q8)
{
    (void)opaque;
    return mtfs_ra8p1_sentinel_monitor_open(&media_context, threshold_q8);
}

static mtfs_error_t monitor_provider_normalize(void *opaque,
    const mtfs_sentinel_feature_v1_t *feature, int8_t input_q4[24])
{
    target_monitor_context_t *context = opaque;
    return mtfs_ra8p1_sentinel_monitor_normalize(&media_context, feature,
        context != NULL ? context->injection_active : 0U, input_q4);
}

static void monitor_provider_preprocessing_status(void *opaque,
    mtfs_sentinel_monitor_preprocessing_status_t *status)
{
    (void)opaque;
    mtfs_ra8p1_sentinel_monitor_preprocessing_status(&media_context, status);
}

static mtfs_error_t monitor_provider_npu_infer(void *opaque,
    const int8_t input_q4[24], int8_t output_q4[24],
    mtfs_sentinel_inference_result_t *result, uint32_t *latency_us)
{
    target_monitor_context_t *context = opaque;
    if (context != NULL && context->inject_npu_failure != 0U &&
        context->npu_failure_injected == 0U) {
        context->npu_failure_injected = 1U;
        (void)memset(output_q4, 0, 24U);
        (void)memset(result, 0, sizeof(*result));
        *latency_us = 0U;
        return MTFS_ERROR_NOT_READY;
    }
    return mtfs_ra8p1_sentinel_monitor_npu_infer(&media_context, input_q4,
        output_q4, result, latency_us);
}

static mtfs_error_t monitor_provider_cpu_infer(void *opaque,
    const int8_t input_q4[24], mtfs_sentinel_inference_result_t *result,
    uint32_t *latency_us)
{
    (void)opaque;
    return mtfs_ra8p1_sentinel_monitor_cpu_infer(&media_context, input_q4,
        result, latency_us);
}

static mtfs_error_t monitor_provider_close(void *opaque)
{
    (void)opaque;
    return mtfs_ra8p1_sentinel_monitor_close(&media_context);
}

static const mtfs_sentinel_monitor_provider_ops_t monitor_provider_ops = {
    monitor_provider_open,
    monitor_provider_normalize,
    monitor_provider_npu_infer,
    monitor_provider_cpu_infer,
    monitor_provider_close,
    monitor_provider_preprocessing_status
};

static int run_monitor(uint32_t samples, uint32_t seed, int pseudo,
    int inject_npu_failure, int hotplug)
{
    target_monitor_context_t context;
    mtfs_sentinel_monitor_run_config_t run_config;
    mtfs_sentinel_observer_config_t observer_config;
    mtfs_sentinel_lab_injector_config_t injector_config;
    mtfs_sentinel_config_t sentinel_config;
    mtfs_sentinel_sample_metadata_t metadata;
    mtfs_block_device_t *device = NULL;
    int prepared = 0, observer_ready = 0, registered = 0;
    int failed = 1;

    if (hotplug != 0 && samples > UINT32_MAX - 35U) return 1;

#if MTFS_SENTINEL_LAB_BUILD_RELEASE == 0
    lab_console_write(NULL,
        "# Debug classification is diagnostic/non-normative; "
        "no classification PASS is asserted\r\n"
        "# monitor exit status covers execution integrity only\r\n");
#endif

    (void)memset(&context, 0, sizeof(context));
    (void)memset(&run_config, 0, sizeof(run_config));
    (void)memset(&lab_runtime, 0, sizeof(lab_runtime));
    context.seed = seed;
    context.pseudo = pseudo != 0 ? 1U : 0U;
    context.inject_npu_failure = inject_npu_failure != 0 ? 1U : 0U;
    context.hotplug = hotplug != 0 ? 1U : 0U;
    context.lab_config.platform_context = NULL;
    context.lab_config.clock_us = mtfs_ra8p1_benchmark_clock_us;
    context.lab_config.sentinel_clock = mtfs_ra8p1_sentinel_clock_us;
    context.lab_config.sleep = platform_sleep;
    context.lab_config.write = platform_write;
    context.lab_config.collect_metadata = collect_metadata;
    context.lab_config.build_type = LAB_BUILD_TYPE;
    context.lab_config.target_id = LAB_TARGET_ID;
    context.lab_config.transport_id = LAB_TRANSPORT_ID;
    context.lab_config.light_delay_us = LAB_RA_LIGHT_DELAY_US;
    context.lab_config.medium_delay_us = LAB_RA_MEDIUM_DELAY_US;
    context.lab_config.strong_delay_us = LAB_RA_STRONG_DELAY_US;

    if (platform_prepare(NULL, &device) != MTFS_OK) goto cleanup;
    prepared = 1;
    if (!mtfs_block_device_is_valid(device)) goto cleanup;
    injector_config.downstream = device;
    injector_config.delay = monitor_injected_delay;
    injector_config.delay_context = NULL;
    if (mtfs_sentinel_lab_injector_init(&lab_runtime.injector,
            &injector_config) != MTFS_OK) goto cleanup;
    observer_config.downstream = mtfs_sentinel_lab_injector_block_device(
        &lab_runtime.injector);
    observer_config.clock = context.lab_config.sentinel_clock;
    observer_config.clock_context = NULL;
    observer_config.lock = observer_lock;
    observer_config.unlock = observer_unlock;
    observer_config.lock_context = &observer_mutex_id;
    if (mtfs_sentinel_observer_init(&lab_runtime.observer,
            &observer_config) != MTFS_OK) goto cleanup;
    observer_ready = 1;
    device = mtfs_sentinel_observer_block_device(&lab_runtime.observer);
    if (mtfs_block_initialize(device) != MTFS_OK ||
        mtfs_block_registry_register(0U, device) != MTFS_OK) goto cleanup;
    registered = 1;
    if (f_mount(&lab_runtime.filesystem, "0:", 1U) != FR_OK) goto cleanup;
    context.mounted = 1U;
    sentinel_config.observer = &lab_runtime.observer;
    sentinel_config.clock = context.lab_config.sentinel_clock;
    sentinel_config.clock_context = NULL;
    sentinel_config.target_id = LAB_TARGET_ID;
    sentinel_config.transport_id = LAB_TRANSPORT_ID;
    sentinel_config.transport_sample = transport_sample;
    sentinel_config.transport_context = &sd_context;
    if (mtfs_sentinel_init(&lab_runtime.sentinel, &sentinel_config) != MTFS_OK)
        goto cleanup;
    (void)memset(&metadata, 0, sizeof(metadata));
    collect_metadata(NULL, &metadata);
    (void)mtfs_sentinel_sample(&lab_runtime.sentinel, &metadata,
        &lab_runtime.frame);
    mtfs_sentinel_lab_window_runtime_init(&lab_runtime.window);
    context.window_config.context = &context;
    context.window_config.clock_us = mtfs_ra8p1_benchmark_clock_us;
    context.window_config.sleep = platform_sleep;
    context.window_config.collect_metadata = collect_metadata;
    context.window_config.media_ready = monitor_media_ready;
    context.window_config.sentinel = &lab_runtime.sentinel;
    context.window_config.volume = "0:";
    context.window_config.workload_buffer = lab_runtime.workload_buffer;
    context.window_config.workload_size = sizeof(lab_runtime.workload_buffer);
    context.window_config.interval_ms = 1000U;
    context.window_config.cadence = MTFS_SENTINEL_LAB_CADENCE_ABSOLUTE;
    run_config.context = &context;
    run_config.provider_ops = &monitor_provider_ops;
    run_config.provider_context = &context;
    run_config.acquire = monitor_acquire;
    run_config.stage_begin = monitor_stage_begin;
    run_config.stage_end = monitor_stage_end;
    run_config.stage_name = monitor_stage_name;
    run_config.write = platform_write;
    run_config.stage_count = pseudo != 0 ? 5U : 1U;
    run_config.samples_per_stage = hotplug != 0 ? samples + 35U : samples;
    run_config.warmup_samples = 32U;
    run_config.maximum_q4_error = 0U;
    failed = mtfs_sentinel_monitor_run(&context.monitor, &run_config);
    if (hotplug != 0 && (context.monitor.diagnostics.warmup_windows != 64U ||
        context.monitor.diagnostics.npu_inferences != samples + 1U))
        failed = 1;
    tm_printf((UB *)"[MON-DIAG] open=%u install=%u infer=%u close=%u windows=%u rule=%u warmup=%u ood=%u cpu-arb=%u cpu-fallback=%u injected-safe-failure=%u failures=%u\n",
        context.monitor.diagnostics.open_calls,
        context.monitor.diagnostics.open_calls,
        context.monitor.diagnostics.npu_inferences,
        context.monitor.diagnostics.close_calls,
        context.monitor.diagnostics.windows,
        context.monitor.diagnostics.rule_decisions,
        context.monitor.diagnostics.warmup_windows,
        context.monitor.diagnostics.out_of_distribution,
        context.monitor.diagnostics.cpu_arbitrations,
        context.monitor.diagnostics.cpu_fallbacks,
        context.npu_failure_injected,
        context.monitor.diagnostics.failures);
cleanup:
    mtfs_sentinel_lab_injector_disable(&lab_runtime.injector);
    if (context.mounted != 0U) (void)f_mount(NULL, "0:", 0U);
    if (registered) (void)mtfs_block_registry_unregister(0U);
    if (observer_ready)
        (void)mtfs_sentinel_observer_deinit(&lab_runtime.observer);
    if (prepared) platform_finish(NULL);
    lab_console_write(NULL,
        "# injection=disabled temporary-file-cleanup=complete\r\n");
    return failed;
}
#endif

typedef int (*ra_corpus_runner_t)(uint32_t repeats);

static int run_ra_corpus(uint32_t repeats, ra_corpus_runner_t runner,
    const char *label)
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
        tm_printf((UB *)"[RA1-%s] heartbeat-start=FAIL\n", (UB *)label);
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
    if (f_mount(&validation_filesystem, "0:", 1U) != FR_OK) goto cleanup;
    mounted = 1;
    result = runner(repeats);

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
    tm_printf((UB *)"[RA1-%s] scheduler heartbeat-delta=%u fairness=%s\n",
        (UB *)label,
        tflm_heartbeat - heartbeat_before,
        fairness_ok ? (UB *)"PASS" : (UB *)"FAIL");
    tm_printf((UB *)"[RA1-%s] stack lab-used=%u/%u lab-margin=%u "
        "heartbeat-used=%u/%u heartbeat-margin=%u guard=%s\n",
        (UB *)label,
        lab_used, (unsigned int)sizeof(lab_task_stack),
        (unsigned int)sizeof(lab_task_stack) - lab_used,
        heartbeat_used, (unsigned int)sizeof(heartbeat_task_stack),
        (unsigned int)sizeof(heartbeat_task_stack) - heartbeat_used,
        stack_ok ? (UB *)"PASS" : (UB *)"FAIL");
    if (!cleanup_ok || !fairness_ok || !stack_ok) result = -1;
    tm_printf((UB *)"[RA1-%s] exit=%d cleanup=%s\n", (UB *)label, result,
        cleanup_ok ? (UB *)"PASS" : (UB *)"FAIL");
    return result;
}

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE && MTFS_ENABLE_SEALED_MODEL
typedef struct inference_mount_transition_context
{
    FATFS *filesystem;
    int *mounted;
} inference_mount_transition_context_t;

static mtfs_error_t inference_mount_transition(void *opaque, int present)
{
    inference_mount_transition_context_t *context = opaque;
    FRESULT result;
    if (context == NULL || context->filesystem == NULL ||
        context->mounted == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    if (present) {
        result = f_mount(context->filesystem, "0:", 1U);
        if (result != FR_OK) return MTFS_ERROR_NOT_READY;
        *context->mounted = 1;
    } else {
        result = f_mount(NULL, "0:", 0U);
        if (result != FR_OK) return MTFS_ERROR_IO;
        *context->mounted = 0;
    }
    return MTFS_OK;
}

static int run_inference(const mtfs_sentinel_feature_v1_t *feature,
    uint32_t iterations, int hotplug, int diagnostics)
{
    mtfs_block_device_t *device = NULL;
    FATFS filesystem;
    T_CTSK heartbeat_task;
    ID heartbeat_id = 0;
    uint32_t heartbeat_before = 0U;
    int prepared = 0, registered = 0, mounted = 0, failed = 1;
    int fairness_ok = 1;
    inference_mount_transition_context_t transition_context;

    if (feature == NULL) return 1;
    if (diagnostics) {
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
        if (heartbeat_id <= 0 || tk_sta_tsk(heartbeat_id, 0) < E_OK)
            goto cleanup;
        (void)tk_dly_tsk(2U);
        heartbeat_before = tflm_heartbeat;
    }
    if (platform_prepare(NULL, &device) != MTFS_OK) goto cleanup;
    prepared = 1;
    if (!mtfs_block_device_is_valid(device) ||
        mtfs_block_initialize(device) != MTFS_OK ||
        mtfs_block_registry_register(0U, device) != MTFS_OK) goto cleanup;
    registered = 1;
    if (f_mount(&filesystem, "0:", 1U) != FR_OK) goto cleanup;
    mounted = 1;
    transition_context.filesystem = &filesystem;
    transition_context.mounted = &mounted;
    if (hotplug)
        failed = mtfs_ra8p1_sentinel_inference_hotplug_run(
            &media_context, feature, iterations, inference_mount_transition,
            &transition_context);
    else if (diagnostics)
        failed = mtfs_ra8p1_sentinel_inference_profile_run(
            &media_context, feature, iterations);
    else
        failed = mtfs_ra8p1_sentinel_inference_run(&media_context, feature,
            iterations);
cleanup:
    if (mounted) (void)f_mount(NULL, "0:", 0U);
    if (registered) (void)mtfs_block_registry_unregister(0U);
    if (prepared) platform_finish(NULL);
    if (heartbeat_id > 0) {
        tflm_heartbeat_stop = 1U;
        (void)tk_dly_tsk(2U);
        if (tk_del_tsk(heartbeat_id) < E_OK) {
            (void)tk_ter_tsk(heartbeat_id);
            if (tk_del_tsk(heartbeat_id) < E_OK) failed = 1;
        }
        fairness_ok = tflm_heartbeat > heartbeat_before;
        tm_printf((UB *)"[sentinel-profile] scheduler heartbeat-delta=%u fairness=%s\n",
            tflm_heartbeat - heartbeat_before,
            fairness_ok ? (UB *)"PASS" : (UB *)"FAIL");
        if (!fairness_ok) failed = 1;
    }
    return failed;
}

static int run_inference_vector(const int8_t input_q4[24],
    uint32_t iterations)
{
    mtfs_block_device_t *device = NULL;
    FATFS filesystem;
    int prepared = 0, registered = 0, mounted = 0, failed = 1;
    if (input_q4 == NULL || iterations == 0U) return 1;
    if (platform_prepare(NULL, &device) != MTFS_OK) goto cleanup;
    prepared = 1;
    if (!mtfs_block_device_is_valid(device) ||
        mtfs_block_initialize(device) != MTFS_OK ||
        mtfs_block_registry_register(0U, device) != MTFS_OK) goto cleanup;
    registered = 1;
    if (f_mount(&filesystem, "0:", 1U) != FR_OK) goto cleanup;
    mounted = 1;
    failed = mtfs_ra8p1_sentinel_inference_vector_run(&media_context,
        input_q4, iterations);
cleanup:
    if (mounted) (void)f_mount(NULL, "0:", 0U);
    if (registered) (void)mtfs_block_registry_unregister(0U);
    if (prepared) platform_finish(NULL);
    return failed;
}

static int hex_nibble(char character)
{
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

static int base64url_value(char character)
{
    if (character >= 'A' && character <= 'Z') return character - 'A';
    if (character >= 'a' && character <= 'z') return character - 'a' + 26;
    if (character >= '0' && character <= '9') return character - '0' + 52;
    if (character == '-') return 62;
    if (character == '_') return 63;
    return -1;
}

static int8_t signed_byte(unsigned int value)
{
    return (int8_t)(value < 128U ? (int)value : (int)value - 256);
}

static int parse_inference_vector(const char *line, int8_t input_q4[24],
    uint32_t *iterations)
{
    char command[40], hexadecimal[49], extra;
    unsigned int parsed_iterations = 10U;
    size_t index;
    int fields = sscanf(line, "%39s %48s %u %c", command, hexadecimal,
        &parsed_iterations, &extra);
    if ((fields != 2 && fields != 3) ||
        (strcmp(command, "sentinel-infer-vector") != 0 &&
         strcmp(command, "siv") != 0) ||
        strlen(hexadecimal) != 48U || parsed_iterations == 0U)
        return 0;
    for (index = 0U; index < 24U; ++index) {
        int high = hex_nibble(hexadecimal[index * 2U]);
        int low = hex_nibble(hexadecimal[index * 2U + 1U]);
        unsigned int byte;
        if (high < 0 || low < 0) return 0;
        byte = (unsigned int)high << 4U | (unsigned int)low;
        input_q4[index] = signed_byte(byte);
    }
    *iterations = (uint32_t)parsed_iterations;
    return 1;
}

static int parse_inference_vector_base64(const char *line,
    int8_t input_q4[24], uint32_t *iterations)
{
    char command[40], encoded[33], extra;
    unsigned int parsed_iterations = 10U;
    size_t group;
    int fields = sscanf(line, "%39s %32s %u %c", command, encoded,
        &parsed_iterations, &extra);
    if ((fields != 2 && fields != 3) || strcmp(command, "sivb") != 0 ||
        strlen(encoded) != 32U || parsed_iterations == 0U)
        return 0;
    for (group = 0U; group < 8U; ++group) {
        int a = base64url_value(encoded[group * 4U]);
        int b = base64url_value(encoded[group * 4U + 1U]);
        int c = base64url_value(encoded[group * 4U + 2U]);
        int d = base64url_value(encoded[group * 4U + 3U]);
        unsigned int value;
        if (a < 0 || b < 0 || c < 0 || d < 0) return 0;
        value = (unsigned int)a << 18U | (unsigned int)b << 12U |
            (unsigned int)c << 6U | (unsigned int)d;
        input_q4[group * 3U] = signed_byte((value >> 16U) & 0xffU);
        input_q4[group * 3U + 1U] = signed_byte((value >> 8U) & 0xffU);
        input_q4[group * 3U + 2U] = signed_byte(value & 0xffU);
    }
    *iterations = (uint32_t)parsed_iterations;
    return 1;
}

static int report_lab_task_stack(void)
{
    uint32_t used = stack_high_water(lab_task_stack, sizeof(lab_task_stack));
    uint32_t margin = (uint32_t)sizeof(lab_task_stack) - used;
    int pass = margin >= 256U;
    tm_printf((UB *)"[sentinel-profile] stack-high-water scope=since-boot used=%u size=%u margin=%u guard=%s\n",
        used, (UW)sizeof(lab_task_stack), margin,
        pass ? (UB *)"PASS" : (UB *)"FAIL");
    return pass ? 0 : 1;
}
#endif
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
#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE && MTFS_ENABLE_SEALED_MODEL
    int8_t input_q4[24];
    int diagnostics_failed;
#endif
#endif
    (void)context;
    if (strcmp(line, "help") == 0) {
#if MTFS_ENABLE_STORAGE_SENTINEL
        lab_console_write(NULL,
            "help\r\n"
            "record [samples]\r\n"
            "pseudo-collect-delay-ramp [samples-per-stage] [seed]\r\n"
            "pseudo-collect-hard-fault [samples] [seed]\r\n"
#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE && MTFS_ENABLE_SEALED_MODEL
            "sentinel-monitor [samples]  acquire and classify each live window\r\n"
            "sentinel-monitor-fallback-test [samples]  inject one safe NPU not-ready result\r\n"
            "pseudo-monitor-delay-ramp [samples-per-stage] [seed]  monitor injected delay stages\r\n"
            "sentinel-infer [iterations]  authenticate and compare CPU/NPU on baseline-neutral input\r\n"
            "sentinel-infer-profile [iterations]  timing, scheduler, stack, and Ethos-U diagnostics\r\n"
            "sentinel-infer-pseudo-slow [iterations]  deprecated; use pseudo-monitor-delay-ramp\r\n"
            "sentinel-infer-hotplug [iterations]  resident monitor remove/reinsert/re-warmup\r\n"
            "sentinel-infer-vector HEX48 [iterations]  compare an exact common-Q4 vector\r\n"
            "siv HEX48 [iterations]  short alias for sentinel-infer-vector\r\n"
            "sivb BASE64URL32 [iterations]  compact exact common-Q4 vector\r\n"
#endif
            "sentinel-ra-validate [repeats]  validation-only Ethos-U characterization\r\n"
            "sentinel-ra-accept [repeats]  frozen-contract held-out acceptance\r\n");
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
#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE && MTFS_ENABLE_SEALED_MODEL
    if (parse_command(line, "sentinel-monitor", 10U, 0U,
            &samples, &seed) && samples != 0U && seed == 0U) {
        tm_printf((UB *)"# sentinel-monitor samples=%u\n", samples);
        tm_printf((UB *)"# sentinel_monitor_exit=%d\n",
            run_monitor(samples, 0U, 0, 0, 0));
        return 1;
    }
    if (parse_command(line, "sentinel-monitor-fallback-test", 1U, 0U,
            &samples, &seed) && samples != 0U && seed == 0U) {
        tm_printf((UB *)"# sentinel-monitor-fallback-test samples=%u\n",
            samples);
        tm_printf((UB *)"# sentinel_monitor_fallback_test_exit=%d\n",
            run_monitor(samples, 0U, 0, 1, 0));
        return 1;
    }
    if (parse_command(line, "pseudo-monitor-delay-ramp", 10U, 1U,
            &samples, &seed) && samples != 0U && seed != 0U) {
        tm_printf((UB *)"# pseudo-monitor-delay-ramp samples_per_stage=%u seed=%u\n",
            samples, seed);
        tm_printf((UB *)"# pseudo_monitor_delay_ramp_exit=%d\n",
            run_monitor(samples, seed, 1, 0, 0));
        return 1;
    }
    if (parse_command(line, "sentinel-infer", 10U, 0U,
            &samples, &seed) && samples != 0U && seed == 0U) {
        tm_printf((UB *)"# sentinel-infer iterations=%u exit=%d\n", samples,
            run_inference(&lab_runtime.frame, samples, 0, 0));
        return 1;
    }
    if (parse_command(line, "sentinel-infer-profile", 10U, 0U,
            &samples, &seed) && samples != 0U && seed == 0U) {
        diagnostics_failed = run_inference(&lab_runtime.frame, samples, 0, 1);
        if (report_lab_task_stack() != 0) diagnostics_failed = 1;
        tm_printf((UB *)"# sentinel-infer-profile iterations=%u exit=%d\n",
            samples, diagnostics_failed);
        return 1;
    }
    if (parse_command(line, "sentinel-infer-pseudo-slow", 10U, 0U,
            &samples, &seed) && samples != 0U && seed == 0U) {
        lab_console_write(NULL,
            "# baseline-relative-v2 does not classify a retained raw frame; use pseudo-monitor-delay-ramp\r\n"
            "# sentinel-infer-pseudo-slow exit=1\r\n");
        return 1;
    }
    if (parse_command(line, "sentinel-infer-hotplug", 10U, 0U,
            &samples, &seed) && samples != 0U && seed == 0U) {
        tm_printf((UB *)"# sentinel-infer-hotplug iterations=%u exit=%d\n",
            samples, run_monitor(samples, 0U, 0, 0, 1));
        return 1;
    }
    if (parse_inference_vector(line, input_q4, &samples)) {
        tm_printf((UB *)"# sentinel-infer-vector iterations=%u exit=%d\n",
            samples, run_inference_vector(input_q4, samples));
        (void)memset(input_q4, 0, sizeof(input_q4));
        return 1;
    }
    if (parse_inference_vector_base64(line, input_q4, &samples)) {
        tm_printf((UB *)"# sivb iterations=%u exit=%d\n", samples,
            run_inference_vector(input_q4, samples));
        (void)memset(input_q4, 0, sizeof(input_q4));
        return 1;
    }
#endif
    if (parse_command(line, "sentinel-ra-validate", 2U, 0U,
            &samples, &seed) && samples != 0U && seed == 0U) {
        tm_printf((UB *)"# sentinel-ra-validate repeats=%u\n", samples);
        tm_printf((UB *)"# sentinel_ra_validate_exit=%d\n",
            run_ra_corpus(samples, mtfs_ra8p1_validation_run,
                "VALIDATION"));
        return 1;
    }
    if (parse_command(line, "sentinel-ra-accept", 2U, 0U,
            &samples, &seed) && samples != 0U && seed == 0U) {
        tm_printf((UB *)"# sentinel-ra-accept repeats=%u contract=v2\n",
            samples);
        tm_printf((UB *)"# sentinel_ra_accept_exit=%d\n",
            run_ra_corpus(samples, mtfs_ra8p1_acceptance_run,
                "HELDOUT"));
        return 1;
    }
#endif
    return 0;
}

static void lab_task(INT start_code, void *context)
{
    mtfs_sentinel_lab_console_t console;
    int runtime_ready;
    (void)start_code;
    (void)context;
    runtime_ready = mtfs_ra8p1_sentinel_runtime_init();
    mtfs_sentinel_lab_console_init(&console, lab_console_write, NULL,
        lab_command, NULL);
    tm_printf((UB *)"\nmicroT-FS Storage Sentinel Lab\n");
    tm_printf((UB *)"# target: EK-RA8P1\n");
    tm_printf((UB *)"# transport: SPI\n");
    tm_printf((UB *)"# build: %s\n", (UB *)LAB_BUILD_TYPE);
#if MTFS_SENTINEL_LAB_BUILD_RELEASE == 0
    tm_printf((UB *)"# classification quality: Debug diagnostic/non-normative; Release-only reference profile\n");
#endif
#if MTFS_ENABLE_STORAGE_SENTINEL
    tm_printf((UB *)"# feature schema: v%u\n", MTFS_SENTINEL_SCHEMA_VERSION);
    tm_printf((UB *)"# arithmetic: portable-u64-v3\n");
    tm_printf((UB *)"# RA sealed/NPU runtime: %s\n",
        (UB *)(runtime_ready == 0 ? "READY" : "FAIL"));
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
