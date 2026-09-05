#include <stdint.h>
#include <string.h>

#include "mtfs_sentinel_monitor.h"

typedef struct fake_provider
{
    uint32_t opens;
    uint32_t normalizes;
    uint32_t npu_calls;
    uint32_t cpu_calls;
    uint32_t closes;
    mtfs_error_t open_status;
    mtfs_error_t npu_status;
    mtfs_error_t cpu_status;
    int8_t output_value;
    uint8_t cpu_anomaly;
} fake_provider_t;

typedef struct fake_run
{
    fake_provider_t provider;
    uint32_t acquisitions;
    uint32_t fail_at;
    uint32_t writes;
    uint32_t saw_stage_ambiguous;
} fake_run_t;

static mtfs_error_t fake_open(void *opaque, uint64_t *threshold_q8)
{
    fake_provider_t *fake = opaque;
    ++fake->opens;
    *threshold_q8 = UINT64_C(100);
    return fake->open_status;
}

static mtfs_error_t fake_normalize(void *opaque,
    const mtfs_sentinel_feature_v1_t *feature, int8_t input_q4[24])
{
    fake_provider_t *fake = opaque;
    (void)feature;
    ++fake->normalizes;
    (void)memset(input_q4, 0, 24U);
    return MTFS_OK;
}

static mtfs_error_t fake_npu(void *opaque, const int8_t input_q4[24],
    int8_t output_q4[24], mtfs_sentinel_inference_result_t *result,
    uint32_t *latency_us)
{
    fake_provider_t *fake = opaque;
    ++fake->npu_calls;
    if (fake->npu_status != MTFS_OK) return fake->npu_status;
    (void)memset(output_q4, fake->output_value, 24U);
    *latency_us = 79U;
    return mtfs_sentinel_score_q8(input_q4, output_q4, UINT64_C(100), result);
}

static mtfs_error_t fake_cpu(void *opaque, const int8_t input_q4[24],
    mtfs_sentinel_inference_result_t *result, uint32_t *latency_us)
{
    fake_provider_t *fake = opaque;
    (void)input_q4;
    ++fake->cpu_calls;
    if (fake->cpu_status != MTFS_OK) return fake->cpu_status;
    (void)memset(result, 0, sizeof(*result));
    result->score_q8 = fake->cpu_anomaly != 0U ? 101U : 99U;
    result->threshold_q8 = 100U;
    result->anomaly = fake->cpu_anomaly;
    *latency_us = 40U;
    return MTFS_OK;
}

static mtfs_error_t fake_close(void *opaque)
{
    ++((fake_provider_t *)opaque)->closes;
    return MTFS_OK;
}

static const mtfs_sentinel_monitor_provider_ops_t fake_ops = {
    fake_open, fake_normalize, fake_npu, fake_cpu, fake_close
};

static void valid_input(mtfs_sentinel_monitor_input_t *input,
    uint32_t sequence)
{
    (void)memset(input, 0, sizeof(*input));
    input->media_state = MTFS_SENTINEL_MONITOR_MEDIA_PRESENT;
    input->window.sequence = sequence;
    input->window.workload_status = MTFS_OK;
    input->window.sample_status = MTFS_OK;
    input->window.feature.version = MTFS_SENTINEL_SCHEMA_VERSION;
    input->window.feature.struct_size =
        (uint16_t)sizeof(input->window.feature);
    input->window.feature.validity_mask = MTFS_SENTINEL_VALID_REQUIRED;
    input->window.feature.media_generation = 1U;
}

static int test_evaluation(void)
{
    fake_provider_t fake;
    mtfs_sentinel_monitor_t monitor;
    mtfs_sentinel_monitor_input_t input;
    mtfs_sentinel_monitor_result_t result;
    char line[512];
    char small[16];
    (void)memset(&fake, 0, sizeof(fake));
    if (mtfs_sentinel_monitor_open(&monitor, &fake_ops, &fake, 1U) != MTFS_OK)
        return 1;
    valid_input(&input, 1U);
    if (mtfs_sentinel_monitor_evaluate(&monitor, &input, &result) != MTFS_OK ||
        result.state != MTFS_SENTINEL_MONITOR_STATE_NORMAL ||
        result.source != MTFS_SENTINEL_MONITOR_SOURCE_NPU ||
        fake.cpu_calls != 0U || fake.npu_calls != 1U)
        return 1;
    fake.output_value = 10;
    fake.cpu_anomaly = 1U;
    input.window.sequence = 2U;
    if (mtfs_sentinel_monitor_evaluate(&monitor, &input, &result) != MTFS_OK ||
        result.source != MTFS_SENTINEL_MONITOR_SOURCE_CPU_ARBITRATION ||
        result.state != MTFS_SENTINEL_MONITOR_STATE_ANOMALY ||
        result.cpu_arbitrated == 0U || fake.cpu_calls != 1U)
        return 1;
    fake.npu_status = MTFS_ERROR_NOT_READY;
    fake.cpu_anomaly = 0U;
    input.window.sequence = 3U;
    if (mtfs_sentinel_monitor_evaluate(&monitor, &input, &result) != MTFS_OK ||
        result.source != MTFS_SENTINEL_MONITOR_SOURCE_CPU_FALLBACK ||
        result.state != MTFS_SENTINEL_MONITOR_STATE_NORMAL ||
        result.inference_status != MTFS_ERROR_NOT_READY || fake.cpu_calls != 2U)
        return 1;
    fake.npu_status = MTFS_OK;
    valid_input(&input, 4U);
    input.media_state = MTFS_SENTINEL_MONITOR_MEDIA_ABSENT;
    if (mtfs_sentinel_monitor_evaluate(&monitor, &input, &result) != MTFS_OK ||
        result.state != MTFS_SENTINEL_MONITOR_STATE_NO_MEDIA ||
        result.source != MTFS_SENTINEL_MONITOR_SOURCE_RULE ||
        fake.npu_calls != 3U)
        return 1;
    valid_input(&input, 5U);
    input.window.feature.media_generation = 2U;
    if (mtfs_sentinel_monitor_evaluate(&monitor, &input, &result) != MTFS_OK ||
        result.state != MTFS_SENTINEL_MONITOR_STATE_DISCONTINUITY)
        return 1;
    valid_input(&input, 6U);
    input.window.feature.media_generation = 2U;
    input.window.feature.flags = MTFS_SENTINEL_FLAG_INSUFFICIENT_DATA;
    if (mtfs_sentinel_monitor_evaluate(&monitor, &input, &result) != MTFS_OK ||
        result.state != MTFS_SENTINEL_MONITOR_STATE_INSUFFICIENT)
        return 1;
    valid_input(&input, 7U);
    input.window.feature.media_generation = 2U;
    input.window.feature.operation[0].failures = 1U;
    if (mtfs_sentinel_monitor_evaluate(&monitor, &input, &result) != MTFS_OK ||
        result.state != MTFS_SENTINEL_MONITOR_STATE_BLOCK_ERROR)
        return 1;
    if (mtfs_sentinel_monitor_format_line(line, sizeof(line), &input,
            &result) != MTFS_OK || strstr(line, "[MON] seq=7") == NULL ||
        strstr(line, "source=RULE") == NULL ||
        mtfs_sentinel_monitor_format_line(small, sizeof(small), &input,
            &result) != MTFS_ERROR_BUFFER_TOO_SMALL ||
        small[sizeof(small) - 1U] != '\0')
        return 1;
    if (mtfs_sentinel_monitor_close(&monitor) != MTFS_OK ||
        fake.opens != 1U || fake.closes != 1U)
        return 1;
    return 0;
}

static mtfs_error_t run_acquire(void *opaque, uint32_t stage,
    uint32_t index, mtfs_sentinel_monitor_input_t *input)
{
    fake_run_t *run = opaque;
    (void)stage; (void)index;
    ++run->acquisitions;
    if (run->fail_at != 0U && run->acquisitions == run->fail_at)
        return MTFS_ERROR_IO;
    valid_input(input, run->acquisitions);
    return MTFS_OK;
}

static void run_write(void *opaque, const char *text)
{
    fake_run_t *run = opaque;
    if (text != NULL && text[0] != '\0') {
        ++run->writes;
        if (strstr(text, "[MON-STAGE]") != NULL &&
            strstr(text, " ambiguous=") != NULL)
            run->saw_stage_ambiguous = 1U;
    }
}

static int test_runner(void)
{
    fake_run_t run;
    mtfs_sentinel_monitor_t monitor;
    mtfs_sentinel_monitor_run_config_t config;
    (void)memset(&run, 0, sizeof(run));
    (void)memset(&config, 0, sizeof(config));
    config.context = &run;
    config.provider_ops = &fake_ops;
    config.provider_context = &run.provider;
    config.acquire = run_acquire;
    config.write = run_write;
    config.stage_count = 1U;
    config.samples_per_stage = 3U;
    config.maximum_q4_error = 1U;
    if (mtfs_sentinel_monitor_run(&monitor, &config) != 0 ||
        run.provider.opens != 1U || run.provider.npu_calls != 3U ||
        run.provider.cpu_calls != 0U || run.provider.closes != 1U ||
        run.writes != 4U || run.saw_stage_ambiguous == 0U)
        return 1;
    (void)memset(&run, 0, sizeof(run));
    run.fail_at = 2U;
    config.context = &run;
    config.provider_context = &run.provider;
    if (mtfs_sentinel_monitor_run(&monitor, &config) == 0 ||
        run.provider.opens != 1U || run.provider.closes != 1U)
        return 1;
    (void)memset(&run, 0, sizeof(run));
    run.provider.open_status = MTFS_ERROR_AUTHENTICATION;
    config.context = &run;
    config.provider_context = &run.provider;
    if (mtfs_sentinel_monitor_run(&monitor, &config) == 0 ||
        run.provider.cpu_calls != 0U || run.provider.closes != 0U ||
        run.acquisitions != 0U)
        return 1;
    return 0;
}

int main(void)
{
    return test_evaluation() != 0 || test_runner() != 0;
}
