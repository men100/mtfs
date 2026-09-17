/** @file mtfs_sentinel_observer.h
 * @brief Passive timing wrapper for Block Device API calls. / Block Device API 呼び出しを透過的にラップし、処理時間を計測する。
 * @details The observer forwards the original operation exactly once and does
 * not issue extra media I/O.
 * / Observer は元の操作を downstream device に1回だけ委譲し、追加の media I/O は発行しない。
 * @ingroup mtfs_sentinel */
#ifndef MTFS_SENTINEL_OBSERVER_H
#define MTFS_SENTINEL_OBSERVER_H

/** @addtogroup mtfs_sentinel
 * @{ */

#include <stdint.h>

#include "../mtfs_config.h"
#include "../block/mtfs_block_device.h"
#include "../block/mtfs_block_diagnostics.h"

#if MTFS_ENABLE_STORAGE_SENTINEL

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_SENTINEL_HISTOGRAM_BUCKETS (22U)
#define MTFS_SENTINEL_OBSERVER_API_VERSION (UINT16_C(1))

typedef mtfs_error_t (*mtfs_sentinel_clock_fn)(void *context, uint64_t *now_us);
typedef mtfs_error_t (*mtfs_sentinel_lock_fn)(void *context);
typedef void (*mtfs_sentinel_unlock_fn)(void *context);

typedef struct mtfs_sentinel_timing
{
    uint64_t sample_count;
    uint64_t total_latency_us;
    uint64_t histogram[MTFS_SENTINEL_HISTOGRAM_BUCKETS];
    uint64_t invalid_samples;
} mtfs_sentinel_timing_t;

#define MTFS_SENTINEL_OBSERVER_FLAG_SATURATED (UINT32_C(1) << 0)

typedef struct mtfs_sentinel_observer_snapshot
{
    uint16_t api_version;
    uint16_t struct_size;
    uint32_t reset_epoch;
    uint32_t flags;
    mtfs_sentinel_timing_t read;
    mtfs_sentinel_timing_t write;
    mtfs_sentinel_timing_t sync;
} mtfs_sentinel_observer_snapshot_t;

typedef struct mtfs_sentinel_observer_config
{
    mtfs_block_device_t *downstream;
    mtfs_sentinel_clock_fn clock;
    void *clock_context;
    mtfs_sentinel_lock_fn lock;
    mtfs_sentinel_unlock_fn unlock;
    void *lock_context;
} mtfs_sentinel_observer_config_t;

typedef struct mtfs_sentinel_observer
{
    mtfs_sentinel_observer_config_t config;
    mtfs_block_device_t block_device;
    mtfs_block_diagnostics_state_t diagnostics;
    mtfs_sentinel_observer_snapshot_t snapshot;
    uint32_t active_calls;
    uint8_t initialized;
} mtfs_sentinel_observer_t;

/* Inclusive upper bounds; the final bucket contains all larger values. */
extern const uint64_t
    mtfs_sentinel_histogram_upper_us[MTFS_SENTINEL_HISTOGRAM_BUCKETS - 1U];

/** @brief Initialize an observer over a downstream device. / downstream device をラップする observer を初期化する。
 * @param observer Caller-owned storage. / 呼び出し側が確保・所有する observer 領域。
 * @param config Borrowed downstream, monotonic clock, and optional lock pair. / 所有権を取得しない downstream device、単調増加クロック、および省略可能な lock/unlock ペア。
 * @return MTFS_OK or validation/attach error. / MTFS_OKまたはvalidation/attach error。
 * @pre downstream must not be this observer's own block device. / downstream に observer 自身の block device を指定してはならない。 */
mtfs_error_t mtfs_sentinel_observer_init(
    mtfs_sentinel_observer_t *observer,
    const mtfs_sentinel_observer_config_t *config);

/** @brief Deinitialize when no forwarded call is active. / downstream への呼び出しが実行中でない場合に observer を終了処理する。
 * @param observer Initialized observer no longer registered. / 登録解除済みの初期化済み observer。
 * @return MTFS_OK or argument/state error. / MTFS_OKまたは引数/state error。 */
mtfs_error_t mtfs_sentinel_observer_deinit(mtfs_sentinel_observer_t *observer);

/** @brief Borrow the wrapper block device. / observer が提供する wrapper block device への非所有参照を取得する。
 * @param observer Initialized observer. / 初期化済みobserver。
 * @return Pointer valid until deinit, or NULL for invalid input. / deinitまで有効なpointer。無効inputではNULL。 */
mtfs_block_device_t *mtfs_sentinel_observer_block_device(
    mtfs_sentinel_observer_t *observer);

/** @brief Copy a coherent timing snapshot without media I/O. / media I/O を発生させず、整合性の取れた timing snapshot を取得する。
 * @param observer Initialized observer. / 初期化済みobserver。
 * @param[out] snapshot Caller output. / caller output。
 * @return MTFS_OK or lock/state error. / MTFS_OKまたはlock/state error。
 * @note Task context only. / Task context からのみ呼び出すこと。 */
mtfs_error_t mtfs_sentinel_observer_get(
    mtfs_sentinel_observer_t *observer,
    mtfs_sentinel_observer_snapshot_t *snapshot);

/** @brief Reset timing counters and advance reset_epoch. / timing counterをresetしreset_epochを進める。
 * @param observer Initialized idle observer. / 初期化済みidle observer。
 * @return MTFS_OK or lock/state error. / MTFS_OKまたはlock/state error。
 * @note No media I/O is issued. / media I/Oは発行しない。 */
mtfs_error_t mtfs_sentinel_observer_reset(mtfs_sentinel_observer_t *observer);

/** @brief Map a latency to the fixed inclusive histogram. / latency に対応する固定ヒストグラムの bucket を求める。
 * @param elapsed_us Latency in microseconds. / microsecond単位latency。
 * @return Bucket index; the last bucket contains all larger values. / bucket index。最大境界値を超える値はすべて最後の bucket に分類される。 */
uint32_t mtfs_sentinel_histogram_bucket(uint64_t elapsed_us);

#ifdef __cplusplus
}
#endif
#endif /* MTFS_ENABLE_STORAGE_SENTINEL */

/** @} */
#endif /* MTFS_SENTINEL_OBSERVER_H */
