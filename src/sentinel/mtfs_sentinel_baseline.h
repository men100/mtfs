/** @file mtfs_sentinel_baseline.h
 * @brief Heapless per-session baseline-relative preprocessing v2. / ヒープを使用しない、セッション単位のbaseline相対preprocessing v2。
 * @ingroup mtfs_sentinel */
#ifndef MTFS_SENTINEL_BASELINE_H
#define MTFS_SENTINEL_BASELINE_H

/** @addtogroup mtfs_sentinel
 * @{ */

#include "../mtfs_config.h"

#if MTFS_ENABLE_STORAGE_SENTINEL

#include <stdint.h>
#include "../mtfs_error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_SENTINEL_BASELINE_API_VERSION (UINT16_C(1))
#define MTFS_SENTINEL_BASELINE_PREPROCESSING_VERSION (UINT16_C(2))
#define MTFS_SENTINEL_BASELINE_FEATURE_COUNT (24U)
#define MTFS_SENTINEL_BASELINE_MAX_WARMUP_WINDOWS (32U)
#define MTFS_SENTINEL_BASELINE_LATENCY_FEATURE_MASK \
    ((UINT32_C(1) << 0) | (UINT32_C(1) << 7) | (UINT32_C(1) << 14))

typedef enum mtfs_sentinel_baseline_state
{
    MTFS_SENTINEL_BASELINE_UNINITIALIZED = 0,
    MTFS_SENTINEL_BASELINE_WARMUP = 1,
    MTFS_SENTINEL_BASELINE_READY = 2,
    MTFS_SENTINEL_BASELINE_INVALID_DISCONTINUOUS = 3
} mtfs_sentinel_baseline_state_t;

typedef struct mtfs_sentinel_baseline_policy
{
    uint16_t api_version;
    uint16_t struct_size;
    uint16_t preprocessing_version;
    uint16_t warmup_windows;
    uint32_t active_feature_mask;
    uint32_t latency_baseline_floor[3];
    uint32_t relative_scale_floor[MTFS_SENTINEL_BASELINE_FEATURE_COUNT];
    uint8_t maximum_saturated_features;
    uint8_t reserved[3];
} mtfs_sentinel_baseline_policy_t;

typedef struct mtfs_sentinel_baseline_result
{
    int8_t input_q4[MTFS_SENTINEL_BASELINE_FEATURE_COUNT];
    uint32_t saturation_mask;
    uint8_t saturation_count;
    uint8_t out_of_distribution;
    uint8_t reserved[2];
} mtfs_sentinel_baseline_result_t;

typedef struct mtfs_sentinel_baseline
{
    mtfs_sentinel_baseline_policy_t policy;
    uint32_t samples[MTFS_SENTINEL_BASELINE_MAX_WARMUP_WINDOWS]
        [MTFS_SENTINEL_BASELINE_FEATURE_COUNT];
    uint32_t baseline[MTFS_SENTINEL_BASELINE_FEATURE_COUNT];
    uint32_t media_generation;
    uint16_t sample_count;
    uint8_t state;
    uint8_t reserved;
} mtfs_sentinel_baseline_t;

/** @brief Validate/copy policy and initialize baseline storage. / policyを検証・コピーし、baseline用領域を初期化する。
 * @param context Caller-owned fixed storage. / 呼び出し側が所有する固定サイズのbaseline用領域。
 * @param policy Version 2 preprocessing policy with 1..32 warmup windows. / warmup window数が1～32のversion 2 preprocessing policy。
 * @return MTFS_OK or validation error. / MTFS_OKまたはvalidation error。 */
mtfs_error_t mtfs_sentinel_baseline_init(mtfs_sentinel_baseline_t *context,
    const mtfs_sentinel_baseline_policy_t *policy);

/** @brief Start warmup for one media generation. / 指定したmedia generationのwarmupを開始する。
 * @param context Initialized baseline. / 初期化済みのbaseline。
 * @param media_generation Nonzero current generation. / 現在のmedia generation。0は指定不可。
 * @return MTFS_OK or validation/state error. / MTFS_OKまたはvalidatio/／state error。 */
mtfs_error_t mtfs_sentinel_baseline_start(mtfs_sentinel_baseline_t *context,
    uint32_t media_generation);

/** @brief Clear samples and return to UNINITIALIZED. / sampleをクリアし、UNINITIALIZED状態に戻す。
 * @param context Baseline storage. / baseline用領域。 */
void mtfs_sentinel_baseline_reset(mtfs_sentinel_baseline_t *context);

/** @brief Mark continuity invalid and clear session samples. / データの連続性を無効として扱い、セッション内のsampleをクリアする。
 * @param context Baseline storage. / baseline用領域。
 * @note Use after hotplug or generation/counter discontinuity, then warm up again. / hotplugやgeneration/counterの不連続を検出した場合に使用し、その後warmupをやり直す。 */
void mtfs_sentinel_baseline_invalidate(mtfs_sentinel_baseline_t *context);

/** @brief Add an eligible warmup window for the same media generation. / 同じmedia generationに対する、有効なwarmup windowを追加する。
 * @param context Warming baseline. / warmup中のbaseline。
 * @param media_generation Must match start. / start時に指定したmedia generationと一致している必要がある。
 * @param raw Fixed 24-element raw feature vector. / 24要素固定長のraw feature vector。
 * @param eligible Nonzero only for valid normal windows. / 正常かつ有効なwindowの場合のみ非0を指定する。
 * @param injection_active Nonzero excludes injected data from baseline. / 非0の場合、injection dataをbaselineの算出対象から除外する。
 * @return MTFS_OK or state/discontinuity/validation error. / MTFS_OKまたはstate/discontinuity/validation error。 */
mtfs_error_t mtfs_sentinel_baseline_observe(mtfs_sentinel_baseline_t *context,
    uint32_t media_generation,
    const uint32_t raw[MTFS_SENTINEL_BASELINE_FEATURE_COUNT],
    uint8_t eligible, uint8_t injection_active);

/** @brief Transform one raw window relative to a READY baseline. / READY状態のbaselineを基準に、1つのraw windowを変換する。
 * @param context READY baseline. / READY状態のbaseline。
 * @param raw Fixed 24-element raw vector. / 24要素固定長のraw vector。
 * @param[out] result Q4 input, saturation mask/count, and OOD flag. / Q4形式の入力、saturation mask/count、およびOOD flag。
 * @return MTFS_OK or state/validation error. / MTFS_OKまたはstate/validation error。 */
mtfs_error_t mtfs_sentinel_baseline_transform(
    const mtfs_sentinel_baseline_t *context,
    const uint32_t raw[MTFS_SENTINEL_BASELINE_FEATURE_COUNT],
    mtfs_sentinel_baseline_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_ENABLE_STORAGE_SENTINEL */

/** @} */
#endif /* MTFS_SENTINEL_BASELINE_H */
