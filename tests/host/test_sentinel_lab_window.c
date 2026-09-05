#include <stdint.h>
#include <string.h>

#include "mtfs_sentinel_lab_window.h"

typedef struct fake_window
{
    uint64_t now_us;
    uint32_t sleeps;
    uint32_t last_sleep_ms;
    uint32_t workloads;
    uint32_t samples;
} fake_window_t;

static fake_window_t *active_fake;

mtfs_error_t mtfs_sentinel_recorder_workload_ex(const char *volume,
    uint32_t marker, void *buffer, uint32_t size,
    mtfs_sentinel_recorder_cleanup_fn before_cleanup, void *cleanup_context)
{
    (void)volume; (void)marker; (void)buffer; (void)size;
    ++active_fake->workloads;
    active_fake->now_us += UINT64_C(100000);
    if (before_cleanup != NULL) before_cleanup(cleanup_context);
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_sample(mtfs_sentinel_context_t *context,
    const mtfs_sentinel_sample_metadata_t *metadata,
    mtfs_sentinel_feature_v1_t *feature)
{
    (void)context; (void)metadata;
    ++active_fake->samples;
    (void)memset(feature, 0, sizeof(*feature));
    feature->version = MTFS_SENTINEL_SCHEMA_VERSION;
    feature->struct_size = (uint16_t)sizeof(*feature);
    return MTFS_OK;
}

static uint64_t fake_clock(void *opaque)
{
    return ((fake_window_t *)opaque)->now_us;
}

static void fake_sleep(void *opaque, uint32_t delay_ms)
{
    fake_window_t *fake = opaque;
    ++fake->sleeps;
    fake->last_sleep_ms = delay_ms;
    fake->now_us += (uint64_t)delay_ms * UINT64_C(1000);
}

static void fake_metadata(void *opaque,
    mtfs_sentinel_sample_metadata_t *metadata)
{
    (void)opaque;
    metadata->media_generation = 1U;
}

int main(void)
{
    fake_window_t fake;
    mtfs_sentinel_context_t sentinel;
    mtfs_sentinel_lab_window_runtime_t runtime;
    mtfs_sentinel_lab_window_config_t config;
    mtfs_sentinel_lab_window_result_t result;
    uint8_t buffer[16];
    (void)memset(&fake, 0, sizeof(fake));
    (void)memset(&sentinel, 0, sizeof(sentinel));
    (void)memset(&config, 0, sizeof(config));
    active_fake = &fake;
    config.context = &fake;
    config.clock_us = fake_clock;
    config.sleep = fake_sleep;
    config.collect_metadata = fake_metadata;
    config.sentinel = &sentinel;
    config.volume = "0:";
    config.workload_buffer = buffer;
    config.workload_size = sizeof(buffer);
    config.interval_ms = 1000U;
    config.cadence = MTFS_SENTINEL_LAB_CADENCE_RELATIVE;
    mtfs_sentinel_lab_window_runtime_init(&runtime);
    if (mtfs_sentinel_lab_window_step(&runtime, &config, 1U, NULL, NULL,
            &result) != MTFS_OK || fake.last_sleep_ms != 1000U ||
        fake.now_us != UINT64_C(1100000) || result.workload_elapsed_us !=
            100000U || result.actual_interval_us != 0U)
        return 1;
    config.cadence = MTFS_SENTINEL_LAB_CADENCE_ABSOLUTE;
    (void)memset(&fake, 0, sizeof(fake));
    mtfs_sentinel_lab_window_runtime_init(&runtime);
    if (mtfs_sentinel_lab_window_step(&runtime, &config, 2U, NULL, NULL,
            &result) != MTFS_OK || fake.last_sleep_ms != 900U ||
        fake.now_us != UINT64_C(1000000) || result.overrun_us != 0U)
        return 1;
    /* A 2.5 s workload misses one full absolute deadline without busy wait. */
    fake.now_us += UINT64_C(2400000);
    if (mtfs_sentinel_lab_window_step(&runtime, &config, 3U, NULL, NULL,
            &result) != MTFS_OK || result.skipped_deadlines != 1U ||
        result.overrun_us != 1500000U || fake.sleeps != 1U)
        return 1;
    (void)memset(&fake, 0, sizeof(fake));
    fake.now_us = UINT64_MAX - UINT64_C(500000);
    mtfs_sentinel_lab_window_runtime_init(&runtime);
    if (mtfs_sentinel_lab_window_step(&runtime, &config, 4U, NULL, NULL,
            &result) != MTFS_OK || fake.last_sleep_ms != 900U ||
        fake.now_us != UINT64_C(499999) || result.overrun_us != 0U)
        return 1;
    return 0;
}
