#include "mtfs_sentinel_lab_window.h"

#include <limits.h>
#include <string.h>

static uint32_t clamp_u64_u32(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static uint64_t interval_us(const mtfs_sentinel_lab_window_config_t *config)
{
    return (uint64_t)config->interval_ms * UINT64_C(1000);
}

static void wait_relative(const mtfs_sentinel_lab_window_config_t *config)
{
    config->sleep(config->context, config->interval_ms);
}

static void wait_absolute(mtfs_sentinel_lab_window_runtime_t *runtime,
    const mtfs_sentinel_lab_window_config_t *config,
    mtfs_sentinel_lab_window_result_t *result)
{
    uint64_t now = config->clock_us(config->context);
    uint64_t period = interval_us(config);
    int64_t relative;
    uint64_t overdue;
    uint64_t skipped;
    uint64_t remaining;
    uint32_t delay_ms;

    relative = (int64_t)(now - runtime->next_deadline_us);
    if (relative < 0) {
        remaining = (uint64_t)(-(relative + 1)) + 1U;
        delay_ms = remaining > (uint64_t)UINT32_MAX * UINT64_C(1000) ?
            UINT32_MAX : (uint32_t)((remaining + UINT64_C(999)) /
                UINT64_C(1000));
        config->sleep(config->context, delay_ms);
        now = config->clock_us(config->context);
        relative = (int64_t)(now - runtime->next_deadline_us);
    }
    if (relative > 0) {
        overdue = (uint64_t)relative;
        result->overrun_us = clamp_u64_u32(overdue);
        skipped = period == 0U ? 0U : overdue / period;
        result->skipped_deadlines = clamp_u64_u32(skipped);
        if (skipped != 0U)
            runtime->next_deadline_us += skipped * period;
    }
    runtime->next_deadline_us += period;
}

void mtfs_sentinel_lab_window_runtime_init(
    mtfs_sentinel_lab_window_runtime_t *runtime)
{
    if (runtime != NULL) (void)memset(runtime, 0, sizeof(*runtime));
}

mtfs_error_t mtfs_sentinel_lab_window_step(
    mtfs_sentinel_lab_window_runtime_t *runtime,
    const mtfs_sentinel_lab_window_config_t *config, uint32_t sequence,
    mtfs_sentinel_recorder_cleanup_fn before_cleanup, void *cleanup_context,
    mtfs_sentinel_lab_window_result_t *result)
{
    mtfs_sentinel_sample_metadata_t metadata;
    uint64_t start_us;
    uint64_t end_us;

    if (runtime == NULL || config == NULL || result == NULL ||
        config->clock_us == NULL || config->sleep == NULL ||
        config->collect_metadata == NULL || config->sentinel == NULL ||
        config->volume == NULL || config->workload_buffer == NULL ||
        config->workload_size == 0U || config->interval_ms == 0U ||
        config->cadence > MTFS_SENTINEL_LAB_CADENCE_ABSOLUTE)
        return MTFS_ERROR_INVALID_ARGUMENT;

    (void)memset(result, 0, sizeof(*result));
    result->sequence = sequence;
    result->configured_interval_us = clamp_u64_u32(interval_us(config));
    start_us = config->clock_us(config->context);
    if (config->cadence == MTFS_SENTINEL_LAB_CADENCE_ABSOLUTE &&
        runtime->deadline_valid == 0U) {
        /* Unsigned addition intentionally preserves a wrapping clock. */
        runtime->next_deadline_us = start_us + interval_us(config);
        runtime->deadline_valid = 1U;
    }
    result->workload_status = config->media_ready == NULL ? MTFS_OK :
        config->media_ready(config->context);
    if (result->workload_status == MTFS_OK)
        result->workload_status = mtfs_sentinel_recorder_workload_ex(
            config->volume, sequence, config->workload_buffer,
            config->workload_size, before_cleanup, cleanup_context);
    end_us = config->clock_us(config->context);
    result->workload_elapsed_us = end_us >= start_us ?
        clamp_u64_u32(end_us - start_us) : UINT32_MAX;

    if (config->cadence == MTFS_SENTINEL_LAB_CADENCE_ABSOLUTE)
        wait_absolute(runtime, config, result);
    else
        wait_relative(config);

    end_us = config->clock_us(config->context);
    if (runtime->previous_sample_us != 0U &&
        end_us >= runtime->previous_sample_us)
        result->actual_interval_us = clamp_u64_u32(
            end_us - runtime->previous_sample_us);
    runtime->previous_sample_us = end_us;
    (void)memset(&metadata, 0, sizeof(metadata));
    config->collect_metadata(config->context, &metadata);
    result->sample_status = mtfs_sentinel_sample(config->sentinel, &metadata,
        &result->feature);
    return result->sample_status;
}
