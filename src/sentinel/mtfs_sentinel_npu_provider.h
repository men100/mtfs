/** @file mtfs_sentinel_npu_provider.h
 * @brief Target-neutral heapless NPU provider boundary. / ターゲット非依存かつヒープを使用しない NPU provider インターフェース。
 * @ingroup mtfs_sentinel */
#ifndef MTFS_SENTINEL_NPU_PROVIDER_H
#define MTFS_SENTINEL_NPU_PROVIDER_H

/** @addtogroup mtfs_sentinel
 * @{ */

#include "mtfs_sentinel_inference.h"

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_SENTINEL_NPU_PROVIDER_API_VERSION (UINT16_C(1))
#define MTFS_SENTINEL_NPU_MAX_ACTUAL_REGIONS (UINT32_C(8))
#define MTFS_SENTINEL_NPU_MAX_FIXED_ZEROIZE_REGIONS (UINT32_C(8))

typedef struct mtfs_sentinel_npu_actual_region
{
    uint16_t kind;
    uint16_t placement;
    uint32_t alignment;
    uint64_t logical_size;
    uint64_t storage_size;
    uint64_t address_or_offset;
} mtfs_sentinel_npu_actual_region_t;

typedef struct mtfs_sentinel_npu_actual_info
{
    uint32_t runtime_abi;
    uint32_t runtime_variant;
    uint32_t runtime_extra;
    uint32_t copy_size;
    uint32_t copy_alignment;
    uint32_t parameters_offset;
    uint32_t parameters_logical_size;
    uint32_t parameters_storage_size;
    uint32_t activation_address;
    uint32_t activation_size;
    uint32_t external_ram_size;
    uint32_t region_count;
    mtfs_sentinel_npu_actual_region_t regions[MTFS_SENTINEL_NPU_MAX_ACTUAL_REGIONS];
    uint8_t runtime_binary_hash[32];
} mtfs_sentinel_npu_actual_info_t;

typedef mtfs_error_t (*mtfs_sentinel_npu_inspect_fn)(void *target,
    const uint8_t *binary, uint32_t binary_size,
    const mtfs_sentinel_runtime_info_t *runtime,
    mtfs_sentinel_npu_actual_info_t *actual);
typedef mtfs_error_t (*mtfs_sentinel_npu_install_fn)(void *target,
    const uint8_t *binary, uint32_t binary_size, void *copy_memory,
    uint32_t copy_size, const mtfs_sentinel_runtime_info_t *runtime);
typedef mtfs_error_t (*mtfs_sentinel_npu_infer_fn)(void *target,
    const int8_t input[MTFS_SENTINEL_FEATURE_DIMENSION],
    int8_t output[MTFS_SENTINEL_FEATURE_DIMENSION], uint32_t timeout_ms);
typedef mtfs_error_t (*mtfs_sentinel_npu_close_fn)(void *target);
typedef mtfs_error_t (*mtfs_sentinel_npu_lock_fn)(void *target,
    uint32_t timeout_ms);
typedef void (*mtfs_sentinel_npu_unlock_fn)(void *target);
typedef void (*mtfs_sentinel_npu_zeroize_fn)(void *target, void *address,
    uint32_t size);
typedef uint32_t (*mtfs_sentinel_npu_cycle_count_fn)(void *context);

/* Diagnostic timing is opt-in and does not alter the normal inference path. */
/* 診断用の時間計測は明示的に有効化した場合のみ行われ、通常の推論処理には影響しない。 */
typedef struct mtfs_sentinel_npu_inference_profile
{
    uint64_t input_requantize_cycles;
    uint64_t target_infer_cycles;
    uint64_t output_requantize_cycles;
    uint64_t score_decision_cycles;
    uint64_t total_cycles;
    uint32_t attempted;
    uint32_t completed;
} mtfs_sentinel_npu_inference_profile_t;

typedef struct mtfs_sentinel_npu_provider_ops
{
    mtfs_sentinel_npu_inspect_fn inspect;
    mtfs_sentinel_npu_install_fn install;
    mtfs_sentinel_npu_infer_fn infer;
    mtfs_sentinel_npu_close_fn close;
    mtfs_sentinel_npu_lock_fn lock;
    mtfs_sentinel_npu_unlock_fn unlock;
    mtfs_sentinel_npu_zeroize_fn zeroize;
} mtfs_sentinel_npu_provider_ops_t;

typedef struct mtfs_sentinel_npu_provider_config
{
    uint16_t api_version;
    uint16_t struct_size;
    uint32_t provider_id;
    uint32_t accelerator_id;
    const mtfs_sentinel_npu_provider_ops_t *ops;
    void *target;
} mtfs_sentinel_npu_provider_config_t;

typedef struct mtfs_sentinel_npu_context
{
    uint16_t api_version;
    uint16_t struct_size;
    mtfs_sentinel_npu_provider_config_t config;
    mtfs_sentinel_runtime_info_t runtime;
    mtfs_sentinel_runtime_policy_result_t policy;
    void *copy_memory;
    uint32_t copy_size;
    void *fixed_zeroize_memory[MTFS_SENTINEL_NPU_MAX_FIXED_ZEROIZE_REGIONS];
    uint32_t fixed_zeroize_size[MTFS_SENTINEL_NPU_MAX_FIXED_ZEROIZE_REGIONS];
    uint32_t fixed_zeroize_count;
    uint64_t threshold_q8;
    uint8_t open;
    uint8_t locked;
    uint8_t reserved[2];
} mtfs_sentinel_npu_context_t;

/** @brief Validate policy, lock provider, and install a resident runtime. / ポリシーを検証し、provider をロックして runtime を常駐状態でインストールする。
 * @param context Caller-owned closed context. / 呼び出し側が所有する、未オープン状態の context。
 * @param config Borrowed configuration retained until close. / 所有権を取得せず参照し、close まで保持される configuration
 * @param bundle Parsed authenticated bundle retained until close. / close まで有効である必要がある、解析済みの認証済み bundle。
 * @param runtime_index Runtime slot. / runtime slot。
 * @param copy_memory Provider copy region. / provider が使用するコピー先領域。
 * @param copy_size Region bytes satisfying alignment/policy. / alignment および policy の要件を満たす領域サイズ (バイト単位)。
 * @param policies Accepted region policies. / 受け入れ可能な region policy。
 * @param policy_count Policy entry count. / policy entry数。
 * @param timeout_ms Lock/install timeout. / lock/install timeout。
 * @return MTFS_OK or policy/size/provider/state error. / MTFS_OKまたはpolicy/size/provider/state error。
 * @post Failure zeroizes owned sensitive regions and releases acquired locks. / 失敗時には、所有する機密領域をゼロクリアし、取得済みのロックを解放する。 */
mtfs_error_t mtfs_sentinel_npu_open(mtfs_sentinel_npu_context_t *context,
    const mtfs_sentinel_npu_provider_config_t *config,
    const mtfs_sentinel_bundle_t *bundle, uint32_t runtime_index,
    void *copy_memory, uint32_t copy_size,
    const mtfs_sentinel_runtime_region_policy_t *policies,
    uint32_t policy_count, uint32_t timeout_ms);

/** @brief Run resident NPU inference and score it. / 常駐中の NPU runtime で推論を実行し、その結果をスコアリングする。
 * @param context Open context. / open context。
 * @param input_q4 Fixed common-Q4 input. / common-Q4 固定小数点入力。
 * @param[out] output_q4 Fixed common-Q4 output. / 固定common-Q4 output。
 * @param timeout_ms Target inference timeout. / target inference timeout。
 * @param[out] result Score and decision. / スコアおよび判定結果。
 * @return MTFS_OK or provider/timeout/state/argument error. / MTFS_OKまたはprovider/timeout/state/引数error。 */
mtfs_error_t mtfs_sentinel_npu_infer(mtfs_sentinel_npu_context_t *context,
    const int8_t input_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
    int8_t output_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
    uint32_t timeout_ms, mtfs_sentinel_inference_result_t *result);

/** @brief Infer and expose canonical and Q4 outputs. / 推論を実行し、canonical 出力と Q4 出力の両方を返す。
 * @param context Open context. / open context。
 * @param input_q4 Common-Q4 input. / common-Q4 input。
 * @param[out] raw_output_int8 Canonical target output. / target から得られる再量子化前の int8 出力。
 * @param[out] output_q4 Requantized common-Q4 output. / common-Q4 形式に再量子化された出力。
 * @param timeout_ms Target timeout. / target timeout。
 * @param[out] result Score and decision. / スコアおよび判定結果。
 * @return MTFS_OK or provider/timeout/state/argument error. / MTFS_OKまたはprovider/timeout/state/引数error。 */
mtfs_error_t mtfs_sentinel_npu_infer_detailed(
    mtfs_sentinel_npu_context_t *context,
    const int8_t input_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
    int8_t raw_output_int8[MTFS_SENTINEL_FEATURE_DIMENSION],
    int8_t output_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
    uint32_t timeout_ms, mtfs_sentinel_inference_result_t *result);

/** @brief Detailed inference with opt-in cycle profiling. / 明示的に有効化可能な cycle profiling 付き詳細推論。
 * @param context Open context. / open context。
 * @param input_q4 Common-Q4 input. / common-Q4 input。
 * @param[out] raw_output_int8 Canonical target output. / canonical target output。
 * @param[out] output_q4 Common-Q4 output. / common-Q4 output。
 * @param timeout_ms Target timeout. / target timeout。
 * @param[out] result Score and decision. / スコアおよび判定結果。
 * @param cycle_count Monotonic wrapping cycle callback. / ラップアラウンドする単調増加 cycle counter の取得 callback。
 * @param cycle_context Callback context. / callback context。
 * @param[out] profile Stage and total cycle counts. / 各処理ステージおよび全体の cycle 数。
 * @return MTFS_OK or provider/profile/state/argument error. / MTFS_OKまたはprovider/profile/state/引数error。
 * @note Profiling is opt-in and does not change the normal inference path. / Profiling は任意であり、通常の推論処理には影響しない。 */
mtfs_error_t mtfs_sentinel_npu_infer_profiled_detailed(
    mtfs_sentinel_npu_context_t *context,
    const int8_t input_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
    int8_t raw_output_int8[MTFS_SENTINEL_FEATURE_DIMENSION],
    int8_t output_q4[MTFS_SENTINEL_FEATURE_DIMENSION],
    uint32_t timeout_ms, mtfs_sentinel_inference_result_t *result,
    mtfs_sentinel_npu_cycle_count_fn cycle_count, void *cycle_context,
    mtfs_sentinel_npu_inference_profile_t *profile);

/** @brief Close, zeroize policy-selected regions, and unlock. / context を close し、policy で指定された領域を zeroize して、ロックを解放する。
 * @param context Open or failed context. / open 済み、または open 処理に失敗した context。
 * @param timeout_ms Provider close timeout. / provider close timeout。
 * @return MTFS_OK or provider close/state error. / MTFS_OKまたはprovider close/state error。
 * @post Caller still owns allocation lifetime. / close 後も、メモリ領域の確保・解放責任は呼び出し側にある。 */
mtfs_error_t mtfs_sentinel_npu_close(mtfs_sentinel_npu_context_t *context,
    uint32_t timeout_ms);

/** @brief Requantize one common-Q4 value to a runtime int8 domain. / 1つの common-Q4 値を runtime の int8 量子化領域へ再量子化する。
 * @param input_q4 Common-Q4 value. / common-Q4値。
 * @param scale_numerator Runtime scale numerator. / runtime の scale 分子。
 * @param scale_shift Runtime scale shift. / runtime の scale shift 値。
 * @param zero_point Runtime zero point. / runtime の zero point。
 * @param[out] output Saturated int8 value. / 飽和処理済みの int8 値。
 * @return MTFS_OK or validation/overflow error. / MTFS_OKまたはvalidation/overflow error。 */
mtfs_error_t mtfs_sentinel_requantize_q4_to_int8(int8_t input_q4,
    uint32_t scale_numerator, uint32_t scale_shift, int8_t zero_point,
    int8_t *output);

/** @brief Requantize one runtime int8 value to common Q4. / 1つの runtime int8 値を common-Q4 へ再量子化する。
 * @param input Runtime int8 value. / runtime int8値。
 * @param scale_numerator Runtime scale numerator. / runtime の scale 分子。
 * @param scale_shift Runtime scale shift. / runtime の scale shift 値。
 * @param zero_point Runtime zero point. / runtime の zero point。
 * @param[out] output_q4 Saturated common-Q4 value. / 飽和処理済みの int8 値。
 * @return MTFS_OK or validation/overflow error. / MTFS_OKまたはvalidation/overflow error。 */
mtfs_error_t mtfs_sentinel_requantize_int8_to_q4(int8_t input,
    uint32_t scale_numerator, uint32_t scale_shift, int8_t zero_point,
    int8_t *output_q4);
#ifdef __cplusplus
}
#endif

/** @} */
#endif
#endif
