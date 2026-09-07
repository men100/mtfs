#include "mtfs_sentinel_monitor.h"

#if MTFS_ENABLE_STORAGE_SENTINEL && MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE

#include <string.h>

typedef struct monitor_writer
{
    char *buffer;
    size_t capacity;
    size_t used;
    uint8_t overflow;
} monitor_writer_t;

static void writer_init(monitor_writer_t *writer, char *buffer,
    size_t capacity)
{
    writer->buffer = buffer;
    writer->capacity = capacity;
    writer->used = 0U;
    writer->overflow = 0U;
    if (capacity != 0U) buffer[0] = '\0';
}

static void writer_char(monitor_writer_t *writer, char value)
{
    if (writer->overflow != 0U) return;
    if (writer->used + 1U >= writer->capacity) {
        writer->overflow = 1U;
        return;
    }
    writer->buffer[writer->used++] = value;
    writer->buffer[writer->used] = '\0';
}

static void writer_text(monitor_writer_t *writer, const char *text)
{
    if (text == NULL) return;
    while (*text != '\0') writer_char(writer, *text++);
}

static void writer_u64(monitor_writer_t *writer, uint64_t value)
{
    char digits[20];
    uint32_t count = 0U;
    do {
        digits[count++] = (char)('0' + value % UINT64_C(10));
        value /= UINT64_C(10);
    } while (value != 0U && count < sizeof(digits));
    while (count != 0U) writer_char(writer, digits[--count]);
}

static void writer_i32(monitor_writer_t *writer, int32_t value)
{
    uint32_t magnitude;
    if (value < 0) {
        writer_char(writer, '-');
        magnitude = (uint32_t)(-(value + 1)) + 1U;
    } else {
        magnitude = (uint32_t)value;
    }
    writer_u64(writer, magnitude);
}

static void writer_hex64(monitor_writer_t *writer, uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    int shift;
    uint8_t started = 0U;
    writer_text(writer, "0x");
    for (shift = 60; shift >= 0; shift -= 4) {
        uint8_t digit = (uint8_t)((value >> (uint32_t)shift) & UINT64_C(15));
        if (digit != 0U || started != 0U || shift == 0) {
            writer_char(writer, digits[digit]);
            started = 1U;
        }
    }
}

static const char *media_name(mtfs_sentinel_monitor_media_state_t state)
{
    if (state == MTFS_SENTINEL_MONITOR_MEDIA_PRESENT) return "PRESENT";
    if (state == MTFS_SENTINEL_MONITOR_MEDIA_ABSENT) return "ABSENT";
    if (state == MTFS_SENTINEL_MONITOR_MEDIA_NOT_READY) return "NOT_READY";
    return "UNKNOWN";
}

static const char *source_name(mtfs_sentinel_monitor_source_t source)
{
    if (source == MTFS_SENTINEL_MONITOR_SOURCE_NPU) return "NPU";
    if (source == MTFS_SENTINEL_MONITOR_SOURCE_CPU_ARBITRATION)
        return "CPU-ARBITRATION";
    if (source == MTFS_SENTINEL_MONITOR_SOURCE_CPU_FALLBACK)
        return "CPU-FALLBACK";
    return "RULE";
}

static const char *state_name(mtfs_sentinel_monitor_state_t state)
{
    static const char *const names[] = {
        "NORMAL", "ANOMALY", "NO_MEDIA", "NOT_READY", "DISCONTINUITY",
        "INSUFFICIENT", "INVALID", "BLOCK_ERROR", "INFERENCE_ERROR",
        "WARMUP", "OOD"
    };
    return state <= MTFS_SENTINEL_MONITOR_STATE_OUT_OF_DISTRIBUTION ?
        names[state] : "INVALID";
}

static void read_preprocessing_status(mtfs_sentinel_monitor_t *monitor,
    mtfs_sentinel_monitor_result_t *result)
{
    mtfs_sentinel_monitor_preprocessing_status_t status;
    (void)memset(&status, 0, sizeof(status));
    if (monitor->ops->preprocessing_status != NULL)
        monitor->ops->preprocessing_status(monitor->provider_context, &status);
    result->baseline_progress = status.baseline_progress;
    result->baseline_required = status.baseline_required;
    result->saturation_mask = status.saturation_mask;
}

static int has_storage_error(const mtfs_sentinel_feature_v1_t *feature)
{
    uint32_t operation;
    if (feature->io_errors != 0U || feature->not_ready_errors != 0U ||
        feature->no_media_errors != 0U || feature->timeout_errors != 0U ||
        feature->write_protected_errors != 0U ||
        feature->out_of_range_errors != 0U || feature->other_errors != 0U)
        return 1;
    for (operation = 0U; operation < MTFS_SENTINEL_OPERATION_COUNT;
            ++operation) {
        if (feature->operation[operation].failures != 0U) return 1;
    }
    return (feature->validity_mask & MTFS_SENTINEL_VALID_TRANSPORT) != 0U &&
        (feature->transport.transport_errors != 0U ||
         feature->transport.transfer_timeouts != 0U ||
         feature->transport.ready_timeouts != 0U ||
         feature->transport.aborts != 0U ||
         feature->transport.clock_errors != 0U);
}

static int apply_rule(mtfs_sentinel_monitor_t *monitor,
    const mtfs_sentinel_monitor_input_t *input,
    mtfs_sentinel_monitor_result_t *result)
{
    const mtfs_sentinel_feature_v1_t *feature = &input->window.feature;
    mtfs_sentinel_monitor_state_t state;
    int decided = 1;

    if (input->media_state == MTFS_SENTINEL_MONITOR_MEDIA_ABSENT ||
        input->window.workload_status == MTFS_ERROR_NO_MEDIA) {
        state = MTFS_SENTINEL_MONITOR_STATE_NO_MEDIA;
    } else if (input->media_state == MTFS_SENTINEL_MONITOR_MEDIA_NOT_READY ||
        input->window.workload_status == MTFS_ERROR_NOT_READY) {
        state = MTFS_SENTINEL_MONITOR_STATE_NOT_READY;
    } else if (input->window.workload_status != MTFS_OK) {
        state = MTFS_SENTINEL_MONITOR_STATE_BLOCK_ERROR;
    } else if (input->window.sample_status != MTFS_OK ||
        feature->version != MTFS_SENTINEL_SCHEMA_VERSION ||
        feature->struct_size != sizeof(*feature)) {
        state = MTFS_SENTINEL_MONITOR_STATE_INVALID;
    } else if (feature->no_media_errors != 0U) {
        state = MTFS_SENTINEL_MONITOR_STATE_NO_MEDIA;
    } else if (feature->not_ready_errors != 0U) {
        state = MTFS_SENTINEL_MONITOR_STATE_NOT_READY;
    } else if (monitor->previous_generation_valid != 0U &&
        feature->media_generation != monitor->previous_media_generation) {
        state = MTFS_SENTINEL_MONITOR_STATE_DISCONTINUITY;
    } else if ((feature->flags & MTFS_SENTINEL_FLAG_DISCONTINUITY) != 0U) {
        state = MTFS_SENTINEL_MONITOR_STATE_DISCONTINUITY;
    } else if ((feature->flags & MTFS_SENTINEL_FLAG_INSUFFICIENT_DATA) != 0U) {
        state = MTFS_SENTINEL_MONITOR_STATE_INSUFFICIENT;
    } else if ((feature->flags & MTFS_SENTINEL_FLAG_COUNTER_SATURATED) != 0U) {
        state = MTFS_SENTINEL_MONITOR_STATE_INVALID;
    } else if ((feature->validity_mask & MTFS_SENTINEL_VALID_REQUIRED) !=
            MTFS_SENTINEL_VALID_REQUIRED) {
        state = MTFS_SENTINEL_MONITOR_STATE_INVALID;
    } else if (has_storage_error(feature)) {
        state = MTFS_SENTINEL_MONITOR_STATE_BLOCK_ERROR;
    } else {
        decided = 0;
        state = MTFS_SENTINEL_MONITOR_STATE_INVALID;
    }
    if (feature->version == MTFS_SENTINEL_SCHEMA_VERSION &&
        feature->struct_size == sizeof(*feature)) {
        monitor->previous_media_generation = feature->media_generation;
        monitor->previous_generation_valid = 1U;
    }
    if (decided) {
        result->state = state;
        result->source = MTFS_SENTINEL_MONITOR_SOURCE_RULE;
        ++monitor->diagnostics.rule_decisions;
    }
    return decided;
}

mtfs_error_t mtfs_sentinel_monitor_open(mtfs_sentinel_monitor_t *monitor,
    const mtfs_sentinel_monitor_provider_ops_t *ops, void *provider_context,
    uint32_t maximum_q4_error)
{
    mtfs_error_t status;
    if (monitor == NULL || ops == NULL || ops->open == NULL ||
        ops->normalize == NULL || ops->npu_infer == NULL ||
        ops->cpu_infer == NULL || ops->close == NULL ||
        maximum_q4_error > 255U)
        return MTFS_ERROR_INVALID_ARGUMENT;
    (void)memset(monitor, 0, sizeof(*monitor));
    monitor->ops = ops;
    monitor->provider_context = provider_context;
    monitor->maximum_q4_error = maximum_q4_error;
    ++monitor->diagnostics.open_calls;
    status = ops->open(provider_context, &monitor->threshold_q8);
    if (status != MTFS_OK) return status;
    monitor->open = 1U;
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_monitor_evaluate(mtfs_sentinel_monitor_t *monitor,
    const mtfs_sentinel_monitor_input_t *input,
    mtfs_sentinel_monitor_result_t *result)
{
    int8_t normalized[MTFS_SENTINEL_FEATURE_DIMENSION];
    int8_t npu_output[MTFS_SENTINEL_FEATURE_DIMENSION];
    mtfs_sentinel_inference_result_t npu_result;
    mtfs_sentinel_inference_result_t cpu_result;
    mtfs_sentinel_score_interval_t interval;
    mtfs_error_t status;
    uint32_t latency_us = 0U;

    if (monitor == NULL || input == NULL || result == NULL ||
        monitor->open == 0U) return MTFS_ERROR_INVALID_STATE;
    (void)memset(result, 0, sizeof(*result));
    result->sequence = input->window.sequence;
    result->media_generation = input->window.feature.media_generation;
    result->validity_mask = input->window.feature.validity_mask;
    result->flags = input->window.feature.flags;
    result->threshold_q8 = monitor->threshold_q8;
    result->media_state = input->media_state;
    ++monitor->diagnostics.windows;
    if (apply_rule(monitor, input, result)) goto complete;

    status = monitor->ops->normalize(monitor->provider_context,
        &input->window.feature, normalized);
    read_preprocessing_status(monitor, result);
    if (status == MTFS_ERROR_NOT_READY) {
        result->state = MTFS_SENTINEL_MONITOR_STATE_WARMUP;
        result->source = MTFS_SENTINEL_MONITOR_SOURCE_RULE;
        result->inference_status = status;
        ++monitor->diagnostics.rule_decisions;
        ++monitor->diagnostics.warmup_windows;
        goto complete;
    }
    if (status == MTFS_ERROR_OUT_OF_RANGE) {
        result->state = MTFS_SENTINEL_MONITOR_STATE_OUT_OF_DISTRIBUTION;
        result->source = MTFS_SENTINEL_MONITOR_SOURCE_RULE;
        result->inference_status = status;
        ++monitor->diagnostics.rule_decisions;
        ++monitor->diagnostics.out_of_distribution;
        goto complete;
    }
    if (status != MTFS_OK) {
        result->state = MTFS_SENTINEL_MONITOR_STATE_INFERENCE_ERROR;
        result->source = MTFS_SENTINEL_MONITOR_SOURCE_RULE;
        result->inference_status = status;
        ++monitor->diagnostics.failures;
        goto complete;
    }
    status = monitor->ops->npu_infer(monitor->provider_context, normalized,
        npu_output, &npu_result, &latency_us);
    ++monitor->diagnostics.npu_inferences;
    result->inference_status = status;
    result->inference_latency_us = latency_us;
    if (status != MTFS_OK) {
        uint32_t cpu_latency_us = 0U;
        status = monitor->ops->cpu_infer(monitor->provider_context,
            normalized, &cpu_result, &cpu_latency_us);
        result->source = MTFS_SENTINEL_MONITOR_SOURCE_CPU_FALLBACK;
        result->inference_latency_us += cpu_latency_us;
        ++monitor->diagnostics.cpu_fallbacks;
        if (status != MTFS_OK) {
            result->state = MTFS_SENTINEL_MONITOR_STATE_INFERENCE_ERROR;
            ++monitor->diagnostics.failures;
            goto complete;
        }
        result->score_q8 = cpu_result.score_q8;
        result->score_min_q8 = cpu_result.score_q8;
        result->score_max_q8 = cpu_result.score_q8;
        result->state = cpu_result.anomaly != 0U ?
            MTFS_SENTINEL_MONITOR_STATE_ANOMALY :
            MTFS_SENTINEL_MONITOR_STATE_NORMAL;
        goto complete;
    }
    status = mtfs_sentinel_score_interval_q8(normalized, npu_output,
        monitor->maximum_q4_error, monitor->threshold_q8, &interval);
    if (status != MTFS_OK || npu_result.score_q8 < interval.score_min_q8 ||
        npu_result.score_q8 > interval.score_max_q8) {
        result->state = MTFS_SENTINEL_MONITOR_STATE_INFERENCE_ERROR;
        result->source = MTFS_SENTINEL_MONITOR_SOURCE_RULE;
        result->inference_status = status == MTFS_OK ?
            MTFS_ERROR_INVALID_STATE : status;
        ++monitor->diagnostics.failures;
        goto complete;
    }
    result->score_q8 = npu_result.score_q8;
    result->score_min_q8 = interval.score_min_q8;
    result->score_max_q8 = interval.score_max_q8;
    if (interval.decision_class !=
            MTFS_SENTINEL_DECISION_AMBIGUOUS_CPU_ARBITRATION) {
        result->source = MTFS_SENTINEL_MONITOR_SOURCE_NPU;
        result->state = interval.decision_class ==
            MTFS_SENTINEL_DECISION_DEFINITELY_ANOMALY ?
            MTFS_SENTINEL_MONITOR_STATE_ANOMALY :
            MTFS_SENTINEL_MONITOR_STATE_NORMAL;
        goto complete;
    }
    latency_us = 0U;
    status = monitor->ops->cpu_infer(monitor->provider_context, normalized,
        &cpu_result, &latency_us);
    result->inference_latency_us += latency_us;
    result->inference_status = status;
    result->source = MTFS_SENTINEL_MONITOR_SOURCE_CPU_ARBITRATION;
    result->cpu_arbitrated = 1U;
    ++monitor->diagnostics.cpu_arbitrations;
    if (status != MTFS_OK) {
        result->state = MTFS_SENTINEL_MONITOR_STATE_INFERENCE_ERROR;
        ++monitor->diagnostics.failures;
        goto complete;
    }
    result->score_q8 = cpu_result.score_q8;
    result->state = cpu_result.anomaly != 0U ?
        MTFS_SENTINEL_MONITOR_STATE_ANOMALY :
        MTFS_SENTINEL_MONITOR_STATE_NORMAL;
complete:
    (void)memset(normalized, 0, sizeof(normalized));
    (void)memset(npu_output, 0, sizeof(npu_output));
    (void)memset(&npu_result, 0, sizeof(npu_result));
    (void)memset(&cpu_result, 0, sizeof(cpu_result));
    (void)memset(&interval, 0, sizeof(interval));
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_monitor_close(mtfs_sentinel_monitor_t *monitor)
{
    mtfs_error_t status;
    if (monitor == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    if (monitor->open == 0U) return MTFS_OK;
    status = monitor->ops->close(monitor->provider_context);
    ++monitor->diagnostics.close_calls;
    if (status == MTFS_OK) monitor->open = 0U;
    return status;
}

mtfs_error_t mtfs_sentinel_monitor_format_line(char *buffer, size_t capacity,
    const mtfs_sentinel_monitor_input_t *input,
    const mtfs_sentinel_monitor_result_t *result)
{
    monitor_writer_t writer;
    if (buffer == NULL || capacity == 0U || input == NULL || result == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    writer_init(&writer, buffer, capacity);
    writer_text(&writer, "[MON] seq="); writer_u64(&writer, result->sequence);
    writer_text(&writer, " gen="); writer_u64(&writer,
        result->media_generation);
    writer_text(&writer, " media="); writer_text(&writer,
        media_name(result->media_state));
    writer_text(&writer, " valid="); writer_hex64(&writer,
        result->validity_mask);
    writer_text(&writer, " flags="); writer_hex64(&writer, result->flags);
    writer_text(&writer, " score="); writer_u64(&writer, result->score_q8);
    writer_text(&writer, " interval="); writer_u64(&writer,
        result->score_min_q8); writer_char(&writer, ':');
    writer_u64(&writer, result->score_max_q8);
    writer_text(&writer, " threshold="); writer_u64(&writer,
        result->threshold_q8);
    writer_text(&writer, " state="); writer_text(&writer,
        state_name(result->state));
    writer_text(&writer, " source="); writer_text(&writer,
        source_name(result->source));
    writer_text(&writer, " cpu-arb="); writer_u64(&writer,
        result->cpu_arbitrated);
    writer_text(&writer, " baseline="); writer_u64(&writer,
        result->baseline_progress); writer_char(&writer, '/');
    writer_u64(&writer, result->baseline_required);
    writer_text(&writer, " saturation-mask="); writer_hex64(&writer,
        result->saturation_mask);
    writer_text(&writer, " infer="); writer_i32(&writer,
        result->inference_status);
    writer_text(&writer, " latency-us="); writer_u64(&writer,
        result->inference_latency_us);
    writer_text(&writer, " configured-us="); writer_u64(&writer,
        input->window.configured_interval_us);
    writer_text(&writer, " cadence-us="); writer_u64(&writer,
        input->window.actual_interval_us);
    writer_text(&writer, " overrun-us="); writer_u64(&writer,
        input->window.overrun_us);
    writer_text(&writer, " skipped="); writer_u64(&writer,
        input->window.skipped_deadlines);
    writer_text(&writer, "\r\n");
    return writer.overflow != 0U ? MTFS_ERROR_BUFFER_TOO_SMALL : MTFS_OK;
}

static void write_stage_summary(const mtfs_sentinel_monitor_run_config_t *c,
    uint32_t stage, uint32_t count, uint64_t representative,
    const uint32_t sources[4], uint32_t anomaly, uint32_t normal,
    uint32_t ambiguous, uint32_t ood, uint32_t failed)
{
    char line[320];
    monitor_writer_t writer;
    const char *name = c->stage_name == NULL ? "monitor" :
        c->stage_name(c->context, stage);
    writer_init(&writer, line, sizeof(line));
    writer_text(&writer, "[MON-STAGE] stage="); writer_text(&writer, name);
    writer_text(&writer, " samples="); writer_u64(&writer, count);
    writer_text(&writer, " representative-score-q8=");
    writer_u64(&writer, representative);
    writer_text(&writer, " normal="); writer_u64(&writer, normal);
    writer_text(&writer, " anomaly="); writer_u64(&writer, anomaly);
    writer_text(&writer, " ambiguous="); writer_u64(&writer, ambiguous);
    writer_text(&writer, " ood="); writer_u64(&writer, ood);
    writer_text(&writer, " rule="); writer_u64(&writer,
        sources[MTFS_SENTINEL_MONITOR_SOURCE_RULE]);
    writer_text(&writer, " npu="); writer_u64(&writer,
        sources[MTFS_SENTINEL_MONITOR_SOURCE_NPU]);
    writer_text(&writer, " cpu-arb="); writer_u64(&writer,
        sources[MTFS_SENTINEL_MONITOR_SOURCE_CPU_ARBITRATION]);
    writer_text(&writer, " cpu-fallback="); writer_u64(&writer,
        sources[MTFS_SENTINEL_MONITOR_SOURCE_CPU_FALLBACK]);
    writer_text(&writer, " failed="); writer_u64(&writer, failed);
    writer_text(&writer, "\r\n");
    if (writer.overflow == 0U) c->write(c->context, line);
}

int mtfs_sentinel_monitor_run(mtfs_sentinel_monitor_t *monitor,
    const mtfs_sentinel_monitor_run_config_t *config)
{
    mtfs_sentinel_monitor_input_t input;
    mtfs_sentinel_monitor_result_t result;
    uint32_t stage;
    uint32_t index;
    uint32_t warmup_complete = 0U;
    int failed = 0;
    char line[512];
    mtfs_error_t status;

    if (monitor == NULL || config == NULL || config->provider_ops == NULL ||
        config->acquire == NULL || config->write == NULL ||
        config->stage_count == 0U || config->maximum_q4_error > 255U ||
        config->warmup_samples > 1024U ||
        (config->samples_per_stage == 0U && config->stage_count != 1U))
        return 1;
    status = mtfs_sentinel_monitor_open(monitor, config->provider_ops,
        config->provider_context, config->maximum_q4_error);
    if (status != MTFS_OK) {
        char open_line[96];
        monitor_writer_t writer;
        writer_init(&writer, open_line, sizeof(open_line));
        writer_text(&writer, "[MON] open=FAIL status=");
        writer_i32(&writer, status); writer_text(&writer, "\r\n");
        if (writer.overflow == 0U) config->write(config->context, open_line);
        return 1;
    }
    for (index = 0U; !failed && warmup_complete == 0U &&
            index < config->warmup_samples * 4U; ++index) {
        (void)memset(&input, 0, sizeof(input));
        status = config->acquire(config->context, UINT32_MAX, index, &input);
        if (status != MTFS_OK) { failed = 1; break; }
        status = mtfs_sentinel_monitor_evaluate(monitor, &input, &result);
        if (status != MTFS_OK ||
            result.state == MTFS_SENTINEL_MONITOR_STATE_INFERENCE_ERROR)
            failed = 1;
        if (result.state == MTFS_SENTINEL_MONITOR_STATE_WARMUP &&
            result.baseline_required == config->warmup_samples &&
            result.baseline_progress == result.baseline_required)
            warmup_complete = 1U;
        if (mtfs_sentinel_monitor_format_line(line, sizeof(line), &input,
                &result) != MTFS_OK) failed = 1;
        else config->write(config->context, line);
    }
    if (config->warmup_samples != 0U) {
        monitor_writer_t writer;
        if (warmup_complete == 0U) failed = 1;
        writer_init(&writer, line, sizeof(line));
        writer_text(&writer, "[MON-WARMUP] attempts=");
        writer_u64(&writer, index);
        writer_text(&writer, " required=");
        writer_u64(&writer, config->warmup_samples);
        writer_text(&writer, " status=");
        writer_text(&writer, failed == 0 && warmup_complete != 0U ?
            "READY" : "FAIL");
        writer_text(&writer, "\r\n");
        if (writer.overflow == 0U) config->write(config->context, line);
    }
    for (stage = 0U; !failed && stage < config->stage_count; ++stage) {
        uint32_t sources[4] = {0U, 0U, 0U, 0U};
        uint32_t anomaly = 0U, normal = 0U, ambiguous = 0U, ood = 0U;
        uint32_t stage_failed = 0U;
        uint64_t representative = 0U;
        uint32_t count = 0U;
        if (config->stage_begin != NULL &&
            config->stage_begin(config->context, stage) != MTFS_OK) {
            failed = 1;
            break;
        }
        for (index = 0U; config->samples_per_stage == 0U ||
                index < config->samples_per_stage; ++index) {
            (void)memset(&input, 0, sizeof(input));
            status = config->acquire(config->context, stage, index, &input);
            if (status != MTFS_OK) { failed = 1; ++stage_failed; break; }
            status = mtfs_sentinel_monitor_evaluate(monitor, &input, &result);
            if (status != MTFS_OK) { failed = 1; ++stage_failed; break; }
            representative = result.score_q8;
            ++count;
            if (result.source <= MTFS_SENTINEL_MONITOR_SOURCE_CPU_FALLBACK)
                ++sources[result.source];
            if (result.state == MTFS_SENTINEL_MONITOR_STATE_ANOMALY) ++anomaly;
            if (result.state == MTFS_SENTINEL_MONITOR_STATE_NORMAL) ++normal;
            if (result.state ==
                    MTFS_SENTINEL_MONITOR_STATE_OUT_OF_DISTRIBUTION) ++ood;
            if (result.cpu_arbitrated != 0U) ++ambiguous;
            if (result.state == MTFS_SENTINEL_MONITOR_STATE_INFERENCE_ERROR) {
                ++stage_failed;
                failed = 1;
            }
            if (mtfs_sentinel_monitor_format_line(line, sizeof(line), &input,
                    &result) != MTFS_OK) { failed = 1; ++stage_failed; break; }
            config->write(config->context, line);
        }
        if (config->stage_end != NULL &&
            config->stage_end(config->context, stage) != MTFS_OK) failed = 1;
        write_stage_summary(config, stage, count, representative, sources,
            anomaly, normal, ambiguous, ood, stage_failed);
    }
    if (mtfs_sentinel_monitor_close(monitor) != MTFS_OK) failed = 1;
    return failed;
}

#else
typedef int mtfs_sentinel_monitor_disabled_translation_unit_t;
#endif
