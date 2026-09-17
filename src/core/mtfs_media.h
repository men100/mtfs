/** @file mtfs_media.h
 * @brief Platform-independent removable-media state machine. / platform非依存のremovable media state machine。
 * @ingroup mtfs_media */
#ifndef MTFS_MEDIA_H
#define MTFS_MEDIA_H

/** @addtogroup mtfs_media
 * @{ */

#include <stdint.h>

#include "../mtfs_config.h"
#include "../mtfs_error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_MEDIA_WAIT_FOREVER (UINT32_MAX)
#define MTFS_MEDIA_DIAGNOSTICS_API_VERSION (UINT16_C(1))
#define MTFS_MEDIA_DIAGNOSTICS_VALID_STATE (UINT32_C(1) << 0)
#define MTFS_MEDIA_DIAGNOSTICS_VALID_STABLE_PRESENT (UINT32_C(1) << 1)
#define MTFS_MEDIA_DIAGNOSTICS_VALID_NOTIFICATION_SEQUENCE (UINT32_C(1) << 2)
#define MTFS_MEDIA_DIAGNOSTICS_VALID_MEDIA_GENERATION (UINT32_C(1) << 3)

typedef enum mtfs_media_state
{
    MTFS_MEDIA_STATE_ABSENT = 0,
    MTFS_MEDIA_STATE_DEBOUNCING_INSERT,
    MTFS_MEDIA_STATE_PRESENT,
    MTFS_MEDIA_STATE_DEBOUNCING_REMOVE,
    MTFS_MEDIA_STATE_ERROR
} mtfs_media_state_t;

typedef enum mtfs_media_event
{
    MTFS_MEDIA_EVENT_INSERTED = 0,
    MTFS_MEDIA_EVENT_REMOVED,
    MTFS_MEDIA_EVENT_ERROR
} mtfs_media_event_t;

typedef enum mtfs_media_active_level
{
    MTFS_MEDIA_ACTIVE_LOW = 0,
    MTFS_MEDIA_ACTIVE_HIGH = 1
} mtfs_media_active_level_t;

/* Return 0/1 for the raw GPIO level and a negative value on read failure. */
/* raw GPIO levelは0または1、読み取り失敗時は負値を返す。 */
typedef int (*mtfs_media_read_signal_fn)(void *opaque);
typedef void (*mtfs_media_event_fn)(
    void *opaque, mtfs_media_event_t event, mtfs_media_state_t state);

typedef struct mtfs_media_config
{
    mtfs_media_read_signal_fn read_signal;
    void *signal_context;
    mtfs_media_event_fn event_callback;
    void *event_context;
    uint32_t debounce_ms;
    mtfs_media_active_level_t active_level;
} mtfs_media_config_t;

typedef struct mtfs_media_diagnostics
{
    uint16_t api_version;
    uint16_t struct_size;
    uint32_t validity_mask;
    uint32_t reset_epoch;
    uint32_t media_generation;
    uint32_t notification_sequence;
    uint32_t state;
    uint8_t stable_present;
    uint8_t reserved[3];
    uint32_t irq_notifications;
    uint32_t manual_notifications;
    uint32_t poll_checks;
    uint32_t debounce_starts;
    uint32_t debounce_rechecks;
    uint32_t inserted_events;
    uint32_t removed_events;
    uint32_t error_events;
} mtfs_media_diagnostics_t;

/* Concrete by design: applications statically allocate this object. */
/* application側で静的確保できるよう、structの完全な定義を意図的に公開している。 */
typedef struct mtfs_media_context
{
    mtfs_media_config_t config;
    volatile uint32_t notification_sequence;
    volatile uint8_t notified_raw_level;
    volatile uint8_t accepting_notifications;
    uint32_t observed_sequence;
    uint32_t debounce_deadline_ms;
    mtfs_media_state_t state;
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_media_diagnostics_t diagnostics;
    uint32_t media_generation;
#endif
    uint8_t stable_present;
    uint8_t candidate_present;
    uint8_t last_polled_raw_level;
    uint8_t debouncing;
    uint8_t initialized;
} mtfs_media_context_t;

/** @brief Initialize a caller-allocated media context. / 呼び出し側が確保したmedia contextを初期化する。
 * @param context Zeroable storage retained through deinit. / deinitまで有効な状態を維持する、zero初期化可能なstorage。
 * @param config Callbacks and debounce policy copied by value. / callbackとdebounce policy。内容はvalue copyされる。
 * @return MTFS_OK or argument/signal-read error. / MTFS_OKまたは引数/signal read error。
 * @post Initial stable state reflects the signal read performed by init. / init時に読み取ったsignalの状態が、初期stable stateに反映される。 */
mtfs_error_t mtfs_media_init(
    mtfs_media_context_t *context, const mtfs_media_config_t *config);

/** @brief Stop accepting ISR/manual notifications. / ISRおよびmanual notificationの受付を停止する。
 * @param context Initialized context. / 初期化済みcontext。
 * @return MTFS_OK or INVALID_ARGUMENT/INVALID_STATE. / MTFS_OKまたはINVALID_ARGUMENT/INVALID_STATE。
 * @warning Call before disabling and deleting the source IRQ. / notification発生源のIRQをdisable/deleteする前に呼び出すこと。 */
mtfs_error_t mtfs_media_stop_notifications(mtfs_media_context_t *context);

/** @brief Deinitialize after notification source shutdown. / notification発生源を停止した後にdeinitする。
 * @param context Initialized context whose IRQ can no longer call notify_isr. / IRQからnotify_isrが呼び出されない状態になっている初期化済みcontext。
 * @return MTFS_OK or state/argument error. / MTFS_OKまたはstate/引数error。 */
mtfs_error_t mtfs_media_deinit(mtfs_media_context_t *context);

/**
 * @brief Record a Card Detect level from ISR context. / ISR contextからCard Detectのraw levelを記録する。
 * @param context Initialized notification-accepting context. / notification受付中の初期化済みcontext。
 * @param raw_level GPIO level 0 or 1. / GPIO level。0または1。
 * @return MTFS_OK, INVALID_ARGUMENT, or INVALID_STATE. / MTFS_OK、INVALID_ARGUMENT、またはINVALID_STATE。
 * @note ISR-safe: only records the raw level and a sequence number.  It never reads GPIO, waits, allocates, locks, or invokes the application callback.
 * / ISR-safe: raw levelとsequence番号を記録するだけで、GPIOの読み取り、wait、memory確保、lock、application callbackの呼び出しは行わない。
 */
mtfs_error_t mtfs_media_notify_isr(
    mtfs_media_context_t *context, int raw_level);

/** @brief Trigger an edge check from task context. / task contextからCard Detectのedge検出処理を開始する。
 * @param context Initialized notification-accepting context. / notification受付中の初期化済みcontext。
 * @return MTFS_OK or state/signal error. / MTFS_OKまたはstate/signal error。
 * @note Calls the configured read_signal; not ISR-safe. / 設定されたread_signalを呼び出すためISR-safeではない。 */
mtfs_error_t mtfs_media_notify(mtfs_media_context_t *context);

/**
 * @brief Advance debounce work in task context. / task contextでdebounce処理を進める。
 * @param context Initialized context. / 初期化済みcontext。
 * @param now_ms Monotonic millisecond tick; wraparound is supported by the state machine. / 単調増加するmillisecond tick。state machineはcounterのwraparoundに対応する。
 * @param[out] next_wait_ms Remaining one-shot delay or MTFS_MEDIA_WAIT_FOREVER. / 次回処理までの残り待ち時間。処理待ちがない場合はMTFS_MEDIA_WAIT_FOREVER。
 * @return MTFS_OK or state/signal/callback-independent processing error. / MTFS_OKまたはstate/signal処理に関するerror。callbackの成否には依存しない。
 * @note Task-context processing. next_wait_ms is the remaining one-shot debounce delay, or MTFS_MEDIA_WAIT_FOREVER when no work is pending.
 * / task contextで処理する。next_wait_msにはdebounce完了までの残り待ち時間を返し、処理待ちがない場合はMTFS_MEDIA_WAIT_FOREVERを返す。
 */
mtfs_error_t mtfs_media_process(
    mtfs_media_context_t *context, uint32_t now_ms,
    uint32_t *next_wait_ms);

/** @brief Poll Card Detect as an explicit task-context fallback. / task contextから明示的に使用するfallbackとしてCard Detectをpollする。
 * @param context Initialized context. / 初期化済みcontext。
 * @param now_ms Monotonic millisecond tick. / 単調増加するmillisecond tick。
 * @param[out] next_wait_ms Next debounce wait. / 次回debounce処理までの待ち時間。
 * @return MTFS_OK or state/signal error. / MTFS_OKまたはstate/signal error。
 * @note Polling is never scheduled implicitly. / pollingが内部で暗黙的にscheduleされることはない。 */
mtfs_error_t mtfs_media_poll(
    mtfs_media_context_t *context, uint32_t now_ms,
    uint32_t *next_wait_ms);

/** @brief Return cached lifecycle state without I/O. / I/Oを行わず、cache済みのmedia lifecycle stateを返す。
 * @param context Initialized context. / 初期化済みcontext。
 * @return Current state, or ERROR for an invalid context. / 現在のstate。contextが無効な場合はERROR。 */
mtfs_media_state_t mtfs_media_state(const mtfs_media_context_t *context);

/** @brief Test cached stable presence without I/O. / I/Oを行わず、cache済みの安定確定したmedia present状態を確認する。
 * @param context Initialized context. / 初期化済みcontext。
 * @return 1 when stably present, otherwise 0. / mediaのpresent状態が安定確定している場合は1、それ以外は0。 */
int mtfs_media_is_present(const mtfs_media_context_t *context);

/** @brief Copy cached lifecycle diagnostics. / cache済みのmedia lifecycle diagnosticsをコピーする。
 * @param context Initialized context. / 初期化済みcontext。
 * @param[out] snapshot 呼び出し側が用意するsnapshotの出力先。
 * @return MTFS_OK or argument/state error. / MTFS_OKまたは引数/state error。
 * @note Task context only; no media or GPIO I/O. / task contextからのみ呼び出すこと。media/GPIO I/Oは行わない。 */
mtfs_error_t mtfs_media_diagnostics_get(
    const mtfs_media_context_t *context,
    mtfs_media_diagnostics_t *snapshot);

/** @brief Reset diagnostics and advance reset_epoch. / diagnosticsをresetし、reset_epochを更新する。
 * @param context Initialized context. / 初期化済みcontext。
 * @return MTFS_OK or argument/state error. / MTFS_OKまたは引数/state error。
 * @note Cached media state and generation are preserved. / cache済みのmedia stateとmedia generationは保持される。 */
mtfs_error_t mtfs_media_diagnostics_reset(mtfs_media_context_t *context);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* MTFS_MEDIA_H */
