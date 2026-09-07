#include <limits.h>
#include <string.h>

#include "mtfs_sentinel_baseline.h"
#include "mtfs_test.h"

static mtfs_sentinel_baseline_policy_t policy(void)
{
    mtfs_sentinel_baseline_policy_t value;
    uint32_t index;
    (void)memset(&value, 0, sizeof(value));
    value.api_version = MTFS_SENTINEL_BASELINE_API_VERSION;
    value.struct_size = sizeof(value);
    value.preprocessing_version =
        MTFS_SENTINEL_BASELINE_PREPROCESSING_VERSION;
    value.warmup_windows = 8U;
    value.active_feature_mask = UINT32_C(0x00ffffff);
    value.latency_baseline_floor[0] = 1U;
    value.latency_baseline_floor[1] = 1U;
    value.latency_baseline_floor[2] = 1U;
    for (index = 0U; index < MTFS_SENTINEL_BASELINE_FEATURE_COUNT; ++index)
        value.relative_scale_floor[index] = 16U;
    value.maximum_saturated_features = 1U;
    return value;
}

int test_sentinel_baseline(mtfs_test_t *test)
{
    mtfs_sentinel_baseline_t context;
    mtfs_sentinel_baseline_policy_t config = policy();
    mtfs_sentinel_baseline_result_t result;
    uint32_t raw[MTFS_SENTINEL_BASELINE_FEATURE_COUNT];
    uint32_t frozen[MTFS_SENTINEL_BASELINE_FEATURE_COUNT];
    uint32_t sample;

    (void)memset(raw, 0, sizeof(raw));
    raw[0] = 100U;
    raw[1] = 500U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_baseline_init(&context, &config) == MTFS_OK &&
            context.state == MTFS_SENTINEL_BASELINE_UNINITIALIZED,
            "baseline init is heapless and uninitialized")) return 1;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_baseline_start(&context, 7U) == MTFS_OK,
            "start baseline generation")) return 1;
    for (sample = 0U; sample < 3U; ++sample)
        (void)mtfs_sentinel_baseline_observe(&context, 7U, raw, 0U, 0U);
    if (!MTFS_TEST_CHECK(test, context.sample_count == 0U,
            "invalid warmup frames are ignored")) return 1;
    for (sample = 0U; sample < 2U; ++sample)
        (void)mtfs_sentinel_baseline_observe(&context, 7U, raw, 1U, 1U);
    if (!MTFS_TEST_CHECK(test, context.sample_count == 0U,
            "injected frames are ignored")) return 1;
    for (sample = 0U; sample < 8U; ++sample) {
        raw[0] = sample == 0U ? 10000U : 100U + sample;
        raw[1] = 500U + sample;
        if (mtfs_sentinel_baseline_observe(&context, 7U, raw, 1U, 0U) !=
                MTFS_OK) return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            context.state == MTFS_SENTINEL_BASELINE_READY &&
            context.baseline[0] == 105U && context.baseline[1] == 504U,
            "median warmup rejects one outlier and enters READY")) return 1;
    (void)memcpy(frozen, context.baseline, sizeof(frozen));
    (void)memset(raw, 0xff, sizeof(raw));
    (void)mtfs_sentinel_baseline_observe(&context, 7U, raw, 1U, 0U);
    if (!MTFS_TEST_CHECK(test,
            memcmp(frozen, context.baseline, sizeof(frozen)) == 0,
            "READY baseline remains frozen")) return 1;
    (void)memcpy(raw, frozen, sizeof(raw));
    raw[0] = 116U; /* +105 permille, rounded Q4=105. */
    raw[1] = 496U; /* -8 absolute permille, Q4=-8. */
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_baseline_transform(&context, raw, &result) ==
                MTFS_OK && result.input_q4[0] == 105 &&
                result.input_q4[1] == -8 && result.saturation_count == 0U,
            "relative transform uses signed round-away arithmetic")) return 1;
    raw[0] = UINT32_MAX;
    raw[1] = 0U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_baseline_transform(&context, raw, &result) ==
                MTFS_OK && result.input_q4[0] == INT8_MAX &&
                result.input_q4[1] == INT8_MIN &&
                result.saturation_count == 2U &&
                result.out_of_distribution == 1U,
            "saturation is counted and classified OOD")) return 1;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_baseline_observe(&context, 8U, raw, 1U, 0U) ==
                MTFS_ERROR_INVALID_STATE &&
                context.state == MTFS_SENTINEL_BASELINE_INVALID_DISCONTINUOUS,
            "generation change invalidates baseline")) return 1;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sentinel_baseline_transform(&context, raw, &result) ==
                MTFS_ERROR_INVALID_STATE,
            "invalid baseline cannot transform")) return 1;
    mtfs_sentinel_baseline_reset(&context);
    if (!MTFS_TEST_CHECK(test,
            context.state == MTFS_SENTINEL_BASELINE_UNINITIALIZED &&
                mtfs_sentinel_baseline_start(&context, 9U) == MTFS_OK,
            "explicit reset permits a new generation")) return 1;
    (void)memset(raw, 0, sizeof(raw));
    for (sample = 0U; sample < 8U; ++sample)
        if (mtfs_sentinel_baseline_observe(&context, 9U, raw, 1U, 0U) !=
                MTFS_OK) return 1;
    raw[0] = 1U;
    if (!MTFS_TEST_CHECK(test,
            context.state == MTFS_SENTINEL_BASELINE_READY &&
            mtfs_sentinel_baseline_transform(&context, raw, &result) ==
                MTFS_OK && result.input_q4[0] == INT8_MAX &&
                result.saturation_mask == UINT32_C(1),
            "zero-latency baseline uses the frozen nonzero denominator floor after re-warmup"))
        return 1;
    return 0;
}
