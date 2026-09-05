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
#include "mtfs_sentinel.h"
#include "mtfs_sentinel_lab_console.h"
#include "mtfs_sentinel_lab_run.h"
#include "mtfs_sentinel_monitor.h"
#include "mtfs_sentinel_recorder.h"
#include "mtfs_stm32_sdmmc.h"
#include "mtfs_stm32n6570_dk_platform.h"
#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE && MTFS_ENABLE_SEALED_MODEL
#include "mtfs_stm32n6570_sentinel_inference.h"
#endif

#define LAB_TARGET_ID (UINT32_C(0x53544e36))
#if MTFS_STM32_SD_USE_IDMA
#define LAB_TRANSPORT_ID (UINT32_C(0x49444d41))
#define LAB_TRANSPORT_NAME "IDMA"
#else
#define LAB_TRANSPORT_ID (UINT32_C(0x504f4c4c))
#define LAB_TRANSPORT_NAME "polling"
#endif
#define LAB_ST_LIGHT_DELAY_US (1000U)
#define LAB_ST_MEDIUM_DELAY_US (3000U)
#define LAB_ST_STRONG_DELAY_US (10000U)
#define LAB_TASK_STACK_SIZE (12U * 1024U)
#define LAB_TASK_STACK_PATTERN (0xa5U)
#define LAB_TASK_STACK_GUARD_SIZE (64U)
#if !defined(MTFS_SENTINEL_LAB_BUILD_RELEASE)
#error "MTFS_SENTINEL_LAB_BUILD_RELEASE must be defined by the build configuration"
#elif MTFS_SENTINEL_LAB_BUILD_RELEASE == 1
#define LAB_BUILD_TYPE "Release"
#elif MTFS_SENTINEL_LAB_BUILD_RELEASE == 0
#define LAB_BUILD_TYPE "Debug"
#else
#error "MTFS_SENTINEL_LAB_BUILD_RELEASE must be 0 or 1"
#endif

#if defined(__GNUC__)
#define LAB_ALIGN8 __attribute__((aligned(8)))
#else
#define LAB_ALIGN8
#endif

static mtfs_stm32_sdmmc_context_t sd_context;
static mtfs_media_context_t media_context;
static mtfs_media_service_context_t media_service;
static uint8_t lab_task_stack[LAB_TASK_STACK_SIZE] LAB_ALIGN8;
#if MTFS_ENABLE_STORAGE_SENTINEL
static ID observer_mutex_id;
static mtfs_sentinel_lab_runtime_t lab_runtime;
#endif

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
        !mtfs_sentinel_operation_timing_is_consistent(&operation))
        return 0;
    operation.latency_histogram[10] = 0U;
    operation.latency_histogram[9] = UINT64_C(1);
    return !mtfs_sentinel_operation_timing_is_consistent(&operation);
}

static mtfs_error_t observer_lock(void *opaque)
{
    return tk_loc_mtx(*(ID *)opaque, TMO_FEVR) >= E_OK ?
        MTFS_OK : MTFS_ERROR_NOT_READY;
}
static void observer_unlock(void *opaque) { (void)tk_unl_mtx(*(ID *)opaque); }

static mtfs_error_t transport_sample(void *opaque,
    mtfs_sentinel_transport_snapshot_t *snapshot)
{
    mtfs_stm32_sdmmc_diagnostics_t d;
    mtfs_error_t error = mtfs_stm32_sdmmc_diagnostics_get(opaque, &d);
    if (error != MTFS_OK) return error;
    snapshot->reset_epoch = d.reset_epoch;
    snapshot->validity_mask =
        MTFS_SENTINEL_TRANSPORT_VALID_TRANSPORT_ERRORS |
        MTFS_SENTINEL_TRANSPORT_VALID_TRANSFER_TIMEOUTS |
        MTFS_SENTINEL_TRANSPORT_VALID_READY_TIMEOUTS |
        MTFS_SENTINEL_TRANSPORT_VALID_ABORTS;
    snapshot->flags = MTFS_SENTINEL_TRANSPORT_FLAG_COUNTERS_SATURATE;
    snapshot->transport_errors = d.error_callbacks;
    snapshot->transfer_timeouts = d.completion_timeouts;
    snapshot->ready_timeouts = d.card_state_timeouts;
    snapshot->aborts = d.aborts;
    if (d.error_callbacks == UINT32_MAX ||
        d.completion_timeouts == UINT32_MAX ||
        d.card_state_timeouts == UINT32_MAX || d.aborts == UINT32_MAX)
        snapshot->flags |=
            MTFS_SENTINEL_TRANSPORT_FLAG_COUNTER_SATURATED;
    return MTFS_OK;
}

static void collect_metadata(void *opaque,
    mtfs_sentinel_sample_metadata_t *metadata)
{
    mtfs_media_diagnostics_t d;
    (void)opaque;
    if (mtfs_media_diagnostics_get(&media_context, &d) == MTFS_OK) {
        metadata->media_generation = d.media_generation;
        metadata->media_reset_epoch = d.reset_epoch;
        metadata->inserted_events = d.inserted_events;
        metadata->removed_events = d.removed_events;
        metadata->error_events = d.error_events;
    }
}
#endif

static void lab_console_write(void *context, const char *text)
{
    INT length = 0;
    (void)context;
    if (text == NULL) return;
    while (text[length] != '\0') ++length;
    if (length != 0) tm_snd_dat((const UB *)text, length);
}

#if MTFS_ENABLE_STORAGE_SENTINEL
static mtfs_error_t platform_prepare(void *opaque,
    mtfs_block_device_t **device)
{
    mtfs_stm32_sdmmc_config_t sd_config;
    T_CMTX mutex = {.mtxatr = TA_INHERIT};
    (void)opaque;
    observer_mutex_id = tk_cre_mtx(&mutex);
    if (observer_mutex_id <= 0) return MTFS_ERROR_NOT_READY;
    mtfs_stm32n6570_dk_sdmmc_config(&sd_config);
    if (mtfs_stm32_sdmmc_context_init(&sd_context, &sd_config) != MTFS_OK) {
        (void)tk_del_mtx(observer_mutex_id);
        observer_mutex_id = 0;
        return MTFS_ERROR_IO;
    }
    if (mtfs_stm32n6570_dk_card_detect_start(&media_context, &media_service,
            &sd_context, NULL, NULL) != MTFS_OK) {
        (void)mtfs_stm32_sdmmc_context_deinit(&sd_context);
        (void)tk_del_mtx(observer_mutex_id);
        observer_mutex_id = 0;
        return MTFS_ERROR_IO;
    }
    *device = mtfs_stm32_sdmmc_block_device(&sd_context);
    return MTFS_OK;
}

static void platform_finish(void *opaque)
{
    (void)opaque;
    (void)mtfs_stm32n6570_dk_card_detect_stop();
    (void)mtfs_stm32_sdmmc_context_deinit(&sd_context);
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
    config.clock_us = mtfs_stm32n6570_dk_benchmark_clock_us;
    config.sentinel_clock = mtfs_stm32n6570_dk_sentinel_clock_us;
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
    config.light_delay_us = LAB_ST_LIGHT_DELAY_US;
    config.medium_delay_us = LAB_ST_MEDIUM_DELAY_US;
    config.strong_delay_us = LAB_ST_STRONG_DELAY_US;
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
} target_monitor_context_t;

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
    return MTFS_OK;
}

static mtfs_error_t monitor_stage_end(void *opaque, uint32_t stage)
{
    (void)opaque; (void)stage;
    mtfs_sentinel_lab_injector_disable(&lab_runtime.injector);
    return MTFS_OK;
}

static const char *monitor_stage_name(void *opaque, uint32_t stage)
{
    static const char *const names[] = {
        "baseline", "light", "medium", "strong", "recovery"
    };
    target_monitor_context_t *context = opaque;
    return context->pseudo != 0U && stage < 5U ? names[stage] : "natural";
}

static mtfs_error_t monitor_acquire(void *opaque, uint32_t stage,
    uint32_t index, mtfs_sentinel_monitor_input_t *input)
{
    target_monitor_context_t *context = opaque;
    (void)stage; (void)index;
    ++context->marker;
    (void)mtfs_sentinel_lab_window_step(&lab_runtime.window,
        &context->window_config, context->marker, NULL, NULL,
        &input->window);
    input->media_state = monitor_media_state();
    if (input->window.sample_status == MTFS_OK)
        lab_runtime.frame = input->window.feature;
    return MTFS_OK;
}

static const mtfs_sentinel_monitor_provider_ops_t monitor_provider_ops = {
    mtfs_stm32n6570_sentinel_monitor_open,
    mtfs_stm32n6570_sentinel_monitor_normalize,
    mtfs_stm32n6570_sentinel_monitor_npu_infer,
    mtfs_stm32n6570_sentinel_monitor_cpu_infer,
    mtfs_stm32n6570_sentinel_monitor_close
};

static int run_monitor(uint32_t samples, uint32_t seed, int pseudo)
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

    (void)memset(&context, 0, sizeof(context));
    (void)memset(&run_config, 0, sizeof(run_config));
    (void)memset(&lab_runtime, 0, sizeof(lab_runtime));
    context.seed = seed;
    context.pseudo = pseudo != 0 ? 1U : 0U;
    context.lab_config.platform_context = NULL;
    context.lab_config.clock_us = mtfs_stm32n6570_dk_benchmark_clock_us;
    context.lab_config.sentinel_clock =
        mtfs_stm32n6570_dk_sentinel_clock_us;
    context.lab_config.sleep = platform_sleep;
    context.lab_config.write = platform_write;
    context.lab_config.collect_metadata = collect_metadata;
    context.lab_config.build_type = LAB_BUILD_TYPE;
    context.lab_config.target_id = LAB_TARGET_ID;
    context.lab_config.transport_id = LAB_TRANSPORT_ID;
    context.lab_config.light_delay_us = LAB_ST_LIGHT_DELAY_US;
    context.lab_config.medium_delay_us = LAB_ST_MEDIUM_DELAY_US;
    context.lab_config.strong_delay_us = LAB_ST_STRONG_DELAY_US;

    if (platform_prepare(NULL, &device) != MTFS_OK) goto cleanup;
    prepared = 1;
    if (!mtfs_block_device_is_valid(device)) goto cleanup;
    injector_config.downstream = device;
    /* Delay injection uses the same non-busy T-Kernel sleep as record. */
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
    context.window_config.clock_us = mtfs_stm32n6570_dk_benchmark_clock_us;
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
    run_config.provider_context = &media_context;
    run_config.acquire = monitor_acquire;
    run_config.stage_begin = monitor_stage_begin;
    run_config.stage_end = monitor_stage_end;
    run_config.stage_name = monitor_stage_name;
    run_config.write = platform_write;
    run_config.stage_count = pseudo != 0 ? 5U : 1U;
    run_config.samples_per_stage = samples;
    run_config.maximum_q4_error = 1U;
    failed = mtfs_sentinel_monitor_run(&context.monitor, &run_config);
    tm_printf((UB *)"[MON-DIAG] open=%u install=%u infer=%u close=%u windows=%u rule=%u cpu-arb=%u cpu-fallback=%u failures=%u\n",
        context.monitor.diagnostics.open_calls,
        context.monitor.diagnostics.open_calls,
        context.monitor.diagnostics.npu_inferences,
        context.monitor.diagnostics.close_calls,
        context.monitor.diagnostics.windows,
        context.monitor.diagnostics.rule_decisions,
        context.monitor.diagnostics.cpu_arbitrations,
        context.monitor.diagnostics.cpu_fallbacks,
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

static int run_inference(const mtfs_sentinel_feature_v1_t *feature,
    uint32_t iterations, int hotplug, int diagnostics)
{
    mtfs_block_device_t *device = NULL;
    FATFS filesystem;
    int prepared = 0, registered = 0, mounted = 0, failed = 1;
    if (feature == NULL || feature->version != MTFS_SENTINEL_SCHEMA_VERSION ||
        feature->struct_size != sizeof(*feature)) {
        lab_console_write(NULL,
            "# no sampled feature; run record 1 or pseudo-collect-delay-ramp first\r\n");
        return 1;
    }
    if (platform_prepare(NULL, &device) != MTFS_OK) goto cleanup;
    prepared = 1;
    if (mtfs_block_initialize(device) != MTFS_OK ||
        mtfs_block_registry_register(0U, device) != MTFS_OK) goto cleanup;
    registered = 1;
    if (f_mount(&filesystem, "0:", 1U) != FR_OK) goto cleanup;
    mounted = 1;
    if (hotplug)
        failed = mtfs_stm32n6570_sentinel_inference_hotplug_run(
            &media_context, feature, iterations);
    else if (diagnostics)
        failed = mtfs_stm32n6570_sentinel_inference_profile_run(
            &media_context, feature, iterations);
    else
        failed = mtfs_stm32n6570_sentinel_inference_run(&media_context,
            feature, iterations);
cleanup:
    if (mounted) (void)f_mount(NULL, "0:", 0U);
    if (registered) (void)mtfs_block_registry_unregister(0U);
    if (prepared) platform_finish(NULL);
    return failed;
}

static int run_inference_vector(const int8_t input_q4[24], uint32_t iterations)
{
    mtfs_block_device_t *device = NULL;
    FATFS filesystem;
    int prepared = 0, registered = 0, mounted = 0, failed = 1;
    if (input_q4 == NULL || iterations == 0U) return 1;
    if (platform_prepare(NULL, &device) != MTFS_OK) goto cleanup;
    prepared = 1;
    if (mtfs_block_initialize(device) != MTFS_OK ||
        mtfs_block_registry_register(0U, device) != MTFS_OK) goto cleanup;
    registered = 1;
    if (f_mount(&filesystem, "0:", 1U) != FR_OK) goto cleanup;
    mounted = 1;
    failed = mtfs_stm32n6570_sentinel_inference_vector_run(
        &media_context, input_q4, iterations);
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
#endif

static int report_lab_task_stack(void)
{
    uint32_t untouched = 0U;
    uint32_t used;
    while (untouched < sizeof(lab_task_stack) &&
        lab_task_stack[untouched] == LAB_TASK_STACK_PATTERN) ++untouched;
    used = (uint32_t)sizeof(lab_task_stack) - untouched;
    tm_printf((UB *)"[sentinel-profile] stack-high-water scope=since-boot used=%u size=%u margin=%u guard=%s\n",
        used, (UW)sizeof(lab_task_stack), untouched,
        untouched >= LAB_TASK_STACK_GUARD_SIZE ? (UB *)"PASS" : (UB *)"FAIL");
    return untouched >= LAB_TASK_STACK_GUARD_SIZE ? 0 : 1;
}

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
            "pseudo-monitor-delay-ramp [samples-per-stage] [seed]  monitor injected delay stages\r\n"
            "sentinel-infer [iterations]  authenticate SENTINEL.MTF and compare CPU/NPU\r\n"
            "sentinel-infer-profile [iterations]  diagnostic timing, scheduler, and stack measurements\r\n"
            "sentinel-infer-pseudo-slow [iterations]  compare retained strong-delay frame\r\n"
            "sentinel-infer-hotplug [iterations]  remove/reinsert SD during resident inference\r\n"
            "sentinel-infer-vector HEX48 [iterations]  compare an exact common-Q4 vector\r\n"
            "siv HEX48 [iterations]  short alias for sentinel-infer-vector\r\n"
            "sivb BASE64URL32 [iterations]  compact exact common-Q4 vector\r\n"
#endif
            );
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
    if (parse_command(line, "sentinel-monitor", 0U, 0U,
            &samples, &seed) && seed == 0U) {
        tm_printf((UB *)"# sentinel-monitor samples=%u\n", samples);
        tm_printf((UB *)"# sentinel_monitor_exit=%d\n",
            run_monitor(samples, 0U, 0));
        return 1;
    }
    if (parse_command(line, "pseudo-monitor-delay-ramp", 10U, 1U,
            &samples, &seed) && samples != 0U && seed != 0U) {
        tm_printf((UB *)"# pseudo-monitor-delay-ramp samples_per_stage=%u seed=%u\n",
            samples, seed);
        tm_printf((UB *)"# pseudo_monitor_delay_ramp_exit=%d\n",
            run_monitor(samples, seed, 1));
        return 1;
    }
    if (parse_command(line, "sentinel-infer", 10U, 0U, &samples, &seed) &&
        samples != 0U && seed == 0U) {
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
        if (lab_runtime.evaluation_frame_valid == 0U)
            lab_console_write(NULL,
                "# no strong-delay frame; run pseudo-collect-delay-ramp first\r\n");
        else
            tm_printf((UB *)"# sentinel-infer-pseudo-slow iterations=%u exit=%d\n",
                samples, run_inference(&lab_runtime.evaluation_frame, samples,
                    0, 0));
        return 1;
    }
    if (parse_command(line, "sentinel-infer-hotplug", 10U, 0U,
            &samples, &seed) && samples != 0U && seed == 0U) {
        tm_printf((UB *)"# sentinel-infer-hotplug iterations=%u exit=%d\n",
            samples, run_inference(&lab_runtime.frame, samples, 1, 0));
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
    tm_printf((UB *)"# target: STM32N6570-DK\n");
    tm_printf((UB *)"# transport: %s\n", (UB *)LAB_TRANSPORT_NAME);
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
    for (;;)
        mtfs_sentinel_lab_console_feed(&console, (char)tm_getchar(1));
}

EXPORT INT usermain(void)
{
    T_CTSK task = {.tskatr=TA_HLNG|TA_RNG3|TA_USERBUF,.task=lab_task,
        .itskpri=9,.stksz=LAB_TASK_STACK_SIZE,.bufptr=lab_task_stack};
    (void)memset(lab_task_stack, LAB_TASK_STACK_PATTERN,
        sizeof(lab_task_stack));
    ID task_id = tk_cre_tsk(&task);
    if ((task_id <= 0) || (tk_sta_tsk(task_id, 0) < E_OK))
        tm_printf((UB *)"# sentinel-lab task start failed\n");
    for (;;) (void)tk_slp_tsk(TMO_FEVR);
    return 0;
}
