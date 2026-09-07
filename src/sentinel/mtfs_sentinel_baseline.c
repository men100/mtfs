#include "mtfs_sentinel_baseline.h"

#if MTFS_ENABLE_STORAGE_SENTINEL

#include <limits.h>
#include <stddef.h>
#include <string.h>

static int policy_valid(const mtfs_sentinel_baseline_policy_t *policy)
{
    uint32_t active;
    uint32_t index;

    if (policy == NULL ||
        policy->api_version != MTFS_SENTINEL_BASELINE_API_VERSION ||
        policy->struct_size != sizeof(*policy) ||
        policy->preprocessing_version !=
            MTFS_SENTINEL_BASELINE_PREPROCESSING_VERSION ||
        (policy->warmup_windows != 8U && policy->warmup_windows != 16U &&
            policy->warmup_windows != 32U) ||
        policy->active_feature_mask == 0U ||
        (policy->active_feature_mask & UINT32_C(0xff000000)) != 0U ||
        policy->maximum_saturated_features >
            MTFS_SENTINEL_BASELINE_FEATURE_COUNT)
        return 0;
    for (index = 0U; index < 3U; ++index) {
        if (policy->latency_baseline_floor[index] == 0U)
            return 0;
    }
    active = policy->active_feature_mask;
    for (index = 0U; index < MTFS_SENTINEL_BASELINE_FEATURE_COUNT; ++index) {
        if ((active & (UINT32_C(1) << index)) != 0U &&
            policy->relative_scale_floor[index] == 0U)
            return 0;
    }
    return 1;
}

static uint32_t median(const mtfs_sentinel_baseline_t *context,
    uint32_t feature)
{
    uint32_t sorted[MTFS_SENTINEL_BASELINE_MAX_WARMUP_WINDOWS];
    uint32_t count = context->policy.warmup_windows;
    uint32_t index;

    for (index = 0U; index < count; ++index)
        sorted[index] = context->samples[index][feature];
    for (index = 1U; index < count; ++index) {
        uint32_t value = sorted[index];
        uint32_t position = index;
        while (position != 0U && sorted[position - 1U] > value) {
            sorted[position] = sorted[position - 1U];
            --position;
        }
        sorted[position] = value;
    }
    return (uint32_t)(((uint64_t)sorted[count / 2U - 1U] +
        sorted[count / 2U] + UINT64_C(1)) / UINT64_C(2));
}

static int64_t divide_round_away(int64_t numerator, uint32_t denominator)
{
    uint64_t magnitude;
    uint64_t rounded;

    if (numerator < 0) {
        magnitude = (uint64_t)(-(numerator + 1)) + UINT64_C(1);
        rounded = (magnitude + denominator / 2U) / denominator;
        return -(int64_t)rounded;
    }
    return (int64_t)(((uint64_t)numerator + denominator / 2U) /
        denominator);
}

static uint32_t latency_slot(uint32_t feature)
{
    return feature == 0U ? 0U : (feature == 7U ? 1U : 2U);
}

mtfs_error_t mtfs_sentinel_baseline_init(mtfs_sentinel_baseline_t *context,
    const mtfs_sentinel_baseline_policy_t *policy)
{
    if (context == NULL || !policy_valid(policy))
        return MTFS_ERROR_INVALID_ARGUMENT;
    (void)memset(context, 0, sizeof(*context));
    context->policy = *policy;
    context->state = MTFS_SENTINEL_BASELINE_UNINITIALIZED;
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_baseline_start(mtfs_sentinel_baseline_t *context,
    uint32_t media_generation)
{
    if (context == NULL || !policy_valid(&context->policy) ||
        media_generation == 0U)
        return MTFS_ERROR_INVALID_ARGUMENT;
    (void)memset(context->samples, 0, sizeof(context->samples));
    (void)memset(context->baseline, 0, sizeof(context->baseline));
    context->media_generation = media_generation;
    context->sample_count = 0U;
    context->state = MTFS_SENTINEL_BASELINE_WARMUP;
    return MTFS_OK;
}

void mtfs_sentinel_baseline_reset(mtfs_sentinel_baseline_t *context)
{
    mtfs_sentinel_baseline_policy_t policy;
    if (context == NULL)
        return;
    policy = context->policy;
    (void)memset(context, 0, sizeof(*context));
    context->policy = policy;
    context->state = MTFS_SENTINEL_BASELINE_UNINITIALIZED;
}

void mtfs_sentinel_baseline_invalidate(mtfs_sentinel_baseline_t *context)
{
    if (context == NULL)
        return;
    (void)memset(context->samples, 0, sizeof(context->samples));
    (void)memset(context->baseline, 0, sizeof(context->baseline));
    context->sample_count = 0U;
    context->state = MTFS_SENTINEL_BASELINE_INVALID_DISCONTINUOUS;
}

mtfs_error_t mtfs_sentinel_baseline_observe(mtfs_sentinel_baseline_t *context,
    uint32_t media_generation,
    const uint32_t raw[MTFS_SENTINEL_BASELINE_FEATURE_COUNT],
    uint8_t eligible, uint8_t injection_active)
{
    uint32_t feature;

    if (context == NULL || raw == NULL || !policy_valid(&context->policy))
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (context->state == MTFS_SENTINEL_BASELINE_UNINITIALIZED ||
        context->state == MTFS_SENTINEL_BASELINE_INVALID_DISCONTINUOUS)
        return MTFS_ERROR_INVALID_STATE;
    if (media_generation == 0U || media_generation != context->media_generation) {
        mtfs_sentinel_baseline_invalidate(context);
        return MTFS_ERROR_INVALID_STATE;
    }
    if (context->state == MTFS_SENTINEL_BASELINE_READY || eligible == 0U ||
        injection_active != 0U)
        return MTFS_OK;
    if (context->sample_count >= context->policy.warmup_windows)
        return MTFS_ERROR_INVALID_STATE;
    for (feature = 0U; feature < MTFS_SENTINEL_BASELINE_FEATURE_COUNT;
            ++feature)
        context->samples[context->sample_count][feature] = raw[feature];
    ++context->sample_count;
    if (context->sample_count == context->policy.warmup_windows) {
        for (feature = 0U; feature < MTFS_SENTINEL_BASELINE_FEATURE_COUNT;
                ++feature)
            context->baseline[feature] = median(context, feature);
        (void)memset(context->samples, 0, sizeof(context->samples));
        context->state = MTFS_SENTINEL_BASELINE_READY;
    }
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_baseline_transform(
    const mtfs_sentinel_baseline_t *context,
    const uint32_t raw[MTFS_SENTINEL_BASELINE_FEATURE_COUNT],
    mtfs_sentinel_baseline_result_t *result)
{
    uint32_t feature;

    if (context == NULL || raw == NULL || result == NULL ||
        !policy_valid(&context->policy))
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (context->state != MTFS_SENTINEL_BASELINE_READY)
        return MTFS_ERROR_INVALID_STATE;
    (void)memset(result, 0, sizeof(*result));
    for (feature = 0U; feature < MTFS_SENTINEL_BASELINE_FEATURE_COUNT;
            ++feature) {
        int64_t relative;
        int64_t scaled;
        uint32_t bit = UINT32_C(1) << feature;
        if ((context->policy.active_feature_mask & bit) == 0U)
            continue;
        relative = (int64_t)raw[feature] - context->baseline[feature];
        if ((MTFS_SENTINEL_BASELINE_LATENCY_FEATURE_MASK & bit) != 0U) {
            uint32_t denominator = context->baseline[feature];
            uint32_t floor = context->policy.latency_baseline_floor[
                latency_slot(feature)];
            if (denominator < floor)
                denominator = floor;
            if (relative > INT64_MAX / INT64_C(1000) ||
                relative < INT64_MIN / INT64_C(1000))
                return MTFS_ERROR_OVERFLOW;
            relative = divide_round_away(relative * INT64_C(1000), denominator);
        }
        if (relative > INT64_MAX / INT64_C(16) ||
            relative < INT64_MIN / INT64_C(16))
            return MTFS_ERROR_OVERFLOW;
        scaled = divide_round_away(relative * INT64_C(16),
            context->policy.relative_scale_floor[feature]);
        if (scaled > INT8_MAX) {
            scaled = INT8_MAX;
            result->saturation_mask |= bit;
            ++result->saturation_count;
        } else if (scaled < INT8_MIN) {
            scaled = INT8_MIN;
            result->saturation_mask |= bit;
            ++result->saturation_count;
        }
        result->input_q4[feature] = (int8_t)scaled;
    }
    result->out_of_distribution = result->saturation_count >
        context->policy.maximum_saturated_features ? 1U : 0U;
    return MTFS_OK;
}

#else
typedef int mtfs_sentinel_baseline_disabled_translation_unit_t;
#endif /* MTFS_ENABLE_STORAGE_SENTINEL */
