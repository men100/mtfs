#include "mtfs_sentinel_lab_run.h"

#include <stdio.h>
#include <string.h>

#include "mtfs_block_registry.h"
#include "mtfs_sentinel_recorder.h"

#define LAB_RECORD_INTERVAL_MS (1000U)

static void write_text(const mtfs_sentinel_lab_run_config_t *config,
    const char *text)
{
    if (config->write != NULL) config->write(config->platform_context, text);
}

static void write_status(const mtfs_sentinel_lab_run_config_t *config,
    const char *format, uint32_t a, uint32_t b, int c)
{
    char line[192];
    int length = snprintf(line, sizeof(line), format, a, b, c);
    if (length > 0 && (size_t)length < sizeof(line)) write_text(config, line);
}

static void injected_delay(void *opaque, uint32_t delay_us)
{
    const mtfs_sentinel_lab_run_config_t *config = opaque;
    uint32_t delay_ms;
    if (delay_us > UINT32_MAX - UINT32_C(999)) {
        delay_ms = UINT32_MAX / UINT32_C(1000);
    } else {
        delay_ms = (delay_us + UINT32_C(999)) / UINT32_C(1000);
    }
    config->sleep(config->platform_context, delay_ms);
}

static void disable_before_cleanup(void *opaque)
{
    mtfs_sentinel_lab_injector_disable(opaque);
}

static const char *mode_command(mtfs_sentinel_lab_mode_t mode)
{
    if (mode == MTFS_SENTINEL_LAB_MODE_DELAY_RAMP)
        return "pseudo-collect-delay-ramp";
    if (mode == MTFS_SENTINEL_LAB_MODE_HARD_FAULT)
        return "pseudo-collect-hard-fault";
    return "record";
}

static uint32_t derived_seed(uint32_t seed, uint32_t discriminator)
{
    uint32_t value = seed ^ discriminator ^ UINT32_C(0x9e3779b9);
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    return value == 0U ? UINT32_C(0x6d2b79f5) : value;
}

static uint32_t stage_count(mtfs_sentinel_lab_mode_t mode)
{
    return mode == MTFS_SENTINEL_LAB_MODE_DELAY_RAMP ? 5U : 1U;
}

static void configure_stage(const mtfs_sentinel_lab_run_config_t *config,
    mtfs_sentinel_lab_runtime_t *runtime, mtfs_sentinel_lab_mode_t mode,
    uint32_t stage, uint32_t seed, mtfs_sentinel_dataset_metadata_t *metadata)
{
    static const char *const delay_stage[] = {
        "baseline", "light", "medium", "strong", "recovery"
    };
    static const uint32_t delay_rate[] = {0U, 50U, 100U, 200U, 0U};
    mtfs_sentinel_lab_injection_t injection;
    uint32_t delay_us = 0U;
    (void)memset(metadata, 0, sizeof(*metadata));
    metadata->label = mode == MTFS_SENTINEL_LAB_MODE_RECORD ?
        "normal" : "evaluation";
    metadata->build_type = config->build_type;
    metadata->command = mode_command(mode);
    metadata->scenario_origin = mode == MTFS_SENTINEL_LAB_MODE_RECORD ?
        "natural" : "injected";
    metadata->stage = mode == MTFS_SENTINEL_LAB_MODE_DELAY_RAMP ?
        delay_stage[stage] : (mode == MTFS_SENTINEL_LAB_MODE_HARD_FAULT ?
        "hard-fault" : "natural");
    metadata->severity = mode == MTFS_SENTINEL_LAB_MODE_DELAY_RAMP &&
        stage < 4U ? stage : (mode == MTFS_SENTINEL_LAB_MODE_HARD_FAULT ?
        4U : 0U);
    metadata->injection_kind = "none";
    metadata->random_seed = mode == MTFS_SENTINEL_LAB_MODE_RECORD ? 0U :
        derived_seed(seed, stage);
    mtfs_sentinel_lab_injector_disable(&runtime->injector);
    mtfs_sentinel_lab_injector_reset_statistics(&runtime->injector);
    if (mode == MTFS_SENTINEL_LAB_MODE_DELAY_RAMP &&
        stage > 0U && stage < 4U) {
        delay_us = stage == 1U ? config->light_delay_us :
            (stage == 2U ? config->medium_delay_us : config->strong_delay_us);
        (void)memset(&injection, 0, sizeof(injection));
        injection.kind = MTFS_SENTINEL_LAB_INJECTION_DELAY;
        injection.operation_mask = MTFS_SENTINEL_LAB_INJECT_ALL;
        injection.rate_permille = delay_rate[stage];
        injection.delay_us = delay_us;
        injection.seed = metadata->random_seed;
        (void)mtfs_sentinel_lab_injector_enable(&runtime->injector,
            &injection);
        metadata->injection_kind = "delay";
        metadata->injection_operation_mask = injection.operation_mask;
        metadata->injection_rate_permille = injection.rate_permille;
        metadata->requested_delay_us = delay_us;
    }
}

static void enable_hard_fault(mtfs_sentinel_lab_runtime_t *runtime,
    mtfs_sentinel_dataset_metadata_t *metadata, uint32_t seed)
{
    mtfs_sentinel_lab_injection_t injection;
    (void)memset(&injection, 0, sizeof(injection));
    injection.kind = MTFS_SENTINEL_LAB_INJECTION_ERROR;
    injection.operation_mask = MTFS_SENTINEL_LAB_INJECT_SYNC;
    injection.rate_permille = 1000U;
    injection.error = MTFS_ERROR_IO;
    injection.seed = seed;
    (void)mtfs_sentinel_lab_injector_enable(&runtime->injector, &injection);
    metadata->injection_kind = "io-error";
    metadata->injection_operation_mask = injection.operation_mask;
    metadata->injection_rate_permille = injection.rate_permille;
    metadata->random_seed = seed;
}

int mtfs_sentinel_lab_run(mtfs_sentinel_lab_runtime_t *runtime,
    const mtfs_sentinel_lab_run_config_t *config,
    mtfs_sentinel_lab_mode_t mode, uint32_t samples, uint32_t seed)
{
    mtfs_sentinel_observer_config_t observer_config;
    mtfs_sentinel_lab_injector_config_t injector_config;
    mtfs_sentinel_config_t sentinel_config;
    mtfs_sentinel_sample_metadata_t sample_metadata;
    mtfs_sentinel_dataset_metadata_t dataset_metadata;
    mtfs_block_device_t *device = NULL;
    mtfs_error_t workload_error;
    uint64_t start_us, end_us;
    uint32_t stage, index, marker = 1U;
    int prepared = 0, observer_ready = 0, registered = 0, mounted = 0;
    int result = 1;

    if (runtime == NULL || config == NULL || config->prepare == NULL ||
        config->finish == NULL || config->clock_us == NULL ||
        config->sentinel_clock == NULL || config->sleep == NULL ||
        config->write == NULL || config->collect_metadata == NULL ||
        config->build_type == NULL || mode > MTFS_SENTINEL_LAB_MODE_HARD_FAULT ||
        (mode != MTFS_SENTINEL_LAB_MODE_RECORD &&
         (samples == 0U || seed == 0U)) ||
        (mode == MTFS_SENTINEL_LAB_MODE_DELAY_RAMP &&
         samples > UINT32_MAX / UINT32_C(5)))
        return 1;
    (void)memset(runtime, 0, sizeof(*runtime));
    if (config->prepare(config->platform_context, &device) != MTFS_OK)
        goto cleanup;
    prepared = 1;
    if (!mtfs_block_device_is_valid(device)) goto cleanup;
    injector_config.downstream = device;
    injector_config.delay = injected_delay;
    injector_config.delay_context = (void *)config;
    if (mtfs_sentinel_lab_injector_init(&runtime->injector,
            &injector_config) != MTFS_OK) goto cleanup;
    observer_config.downstream =
        mtfs_sentinel_lab_injector_block_device(&runtime->injector);
    observer_config.clock = config->sentinel_clock;
    observer_config.clock_context = config->platform_context;
    observer_config.lock = config->observer_lock;
    observer_config.unlock = config->observer_unlock;
    observer_config.lock_context = config->observer_lock_context;
    if (mtfs_sentinel_observer_init(&runtime->observer,
            &observer_config) != MTFS_OK) goto cleanup;
    observer_ready = 1;
    device = mtfs_sentinel_observer_block_device(&runtime->observer);
    if (mtfs_block_initialize(device) != MTFS_OK ||
        mtfs_block_registry_register(0U, device) != MTFS_OK) goto cleanup;
    registered = 1;
    if (f_mount(&runtime->filesystem, "0:", 1U) != FR_OK) goto cleanup;
    mounted = 1;
    sentinel_config.observer = &runtime->observer;
    sentinel_config.clock = config->sentinel_clock;
    sentinel_config.clock_context = config->platform_context;
    sentinel_config.target_id = config->target_id;
    sentinel_config.transport_id = config->transport_id;
    sentinel_config.transport_sample = config->transport_sample;
    sentinel_config.transport_context = config->transport_context;
    if (mtfs_sentinel_init(&runtime->sentinel, &sentinel_config) != MTFS_OK)
        goto cleanup;
    (void)memset(&sample_metadata, 0, sizeof(sample_metadata));
    config->collect_metadata(config->platform_context, &sample_metadata);
    (void)mtfs_sentinel_sample(&runtime->sentinel, &sample_metadata,
        &runtime->frame);
    write_text(config, mtfs_sentinel_recorder_csv_header());
    write_text(config, "\r\n");

    for (stage = 0U; stage < stage_count(mode); ++stage) {
        configure_stage(config, runtime, mode, stage, seed,
            &dataset_metadata);
        write_status(config,
            "# stage=%u rate_permille=%u requested_delay_us=%d\r\n",
            stage, dataset_metadata.injection_rate_permille,
            (int)dataset_metadata.requested_delay_us);
        for (index = 0U; samples == 0U || index < samples; ++index) {
            if (mode == MTFS_SENTINEL_LAB_MODE_HARD_FAULT)
                enable_hard_fault(runtime, &dataset_metadata,
                    derived_seed(seed, index));
            start_us = config->clock_us(config->platform_context);
            workload_error = mtfs_sentinel_recorder_workload_ex("0:", marker,
                runtime->workload_buffer, sizeof(runtime->workload_buffer),
                mode == MTFS_SENTINEL_LAB_MODE_HARD_FAULT ?
                    disable_before_cleanup : NULL,
                &runtime->injector);
            end_us = config->clock_us(config->platform_context);
            write_status(config,
                "# workload-perf marker=%u elapsed_us=%u mtfs=%d\r\n",
                marker, end_us >= start_us && end_us - start_us <= UINT32_MAX ?
                    (uint32_t)(end_us - start_us) : UINT32_MAX,
                (int)workload_error);
            config->sleep(config->platform_context, LAB_RECORD_INTERVAL_MS);
            (void)memset(&sample_metadata, 0, sizeof(sample_metadata));
            config->collect_metadata(config->platform_context,
                &sample_metadata);
            if (mtfs_sentinel_sample(&runtime->sentinel, &sample_metadata,
                    &runtime->frame) == MTFS_OK) {
                if (mode == MTFS_SENTINEL_LAB_MODE_DELAY_RAMP && stage == 3U) {
                    runtime->evaluation_frame = runtime->frame;
                    runtime->evaluation_frame_valid = 1U;
                }
                dataset_metadata.actual_injection_count =
                    mtfs_sentinel_lab_injector_injection_count(
                        &runtime->injector);
                dataset_metadata.sequence = marker;
                dataset_metadata.marker = marker;
                if (mtfs_sentinel_recorder_format_dataset_csv(
                        runtime->csv_line, sizeof(runtime->csv_line),
                        &runtime->frame, &dataset_metadata) == MTFS_OK) {
                    write_text(config, runtime->csv_line);
                    write_text(config, "\r\n");
                }
            }
            ++marker;
        }
    }
    result = 0;

cleanup:
    mtfs_sentinel_lab_injector_disable(&runtime->injector);
    if (mounted) (void)f_mount(NULL, "0:", 0U);
    if (registered) (void)mtfs_block_registry_unregister(0U);
    if (observer_ready) (void)mtfs_sentinel_observer_deinit(&runtime->observer);
    if (prepared) config->finish(config->platform_context);
    write_text(config, "# injection=disabled temporary-file-cleanup=complete\r\n");
    return result;
}
