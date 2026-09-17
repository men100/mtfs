/** @file mtfs_block_diagnostics.h
 * @brief Vendor-independent versioned snapshots. / vendor非依存のversion付きdiagnostic snapshot。
 * @ingroup mtfs_diagnostics */
#ifndef MTFS_BLOCK_DIAGNOSTICS_H
#define MTFS_BLOCK_DIAGNOSTICS_H

/** @addtogroup mtfs_diagnostics
 * @{ */

#include <stdint.h>

#include "../mtfs_config.h"
#include "../mtfs_error.h"
#include "../mtfs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_BLOCK_DIAGNOSTICS_API_VERSION (UINT16_C(1))

typedef enum mtfs_block_operation
{
    MTFS_BLOCK_OPERATION_NONE = 0,
    MTFS_BLOCK_OPERATION_INITIALIZE,
    MTFS_BLOCK_OPERATION_STATUS,
    MTFS_BLOCK_OPERATION_READ,
    MTFS_BLOCK_OPERATION_WRITE,
    MTFS_BLOCK_OPERATION_SYNC,
    MTFS_BLOCK_OPERATION_GET_GEOMETRY,
    MTFS_BLOCK_OPERATION_TRIM
} mtfs_block_operation_t;

#define MTFS_BLOCK_DIAGNOSTICS_VALID_STATUS            (UINT64_C(1) << 0)
#define MTFS_BLOCK_DIAGNOSTICS_VALID_GEOMETRY          (UINT64_C(1) << 1)
#define MTFS_BLOCK_DIAGNOSTICS_VALID_READ_COMPLETED    (UINT64_C(1) << 2)
#define MTFS_BLOCK_DIAGNOSTICS_VALID_WRITE_COMPLETED   (UINT64_C(1) << 3)
#define MTFS_BLOCK_DIAGNOSTICS_VALID_TIMEOUT_COUNTER   (UINT64_C(1) << 4)

#define MTFS_BLOCK_DIAGNOSTICS_FLAG_COUNTERS_SATURATE  (UINT32_C(1) << 0)

typedef struct mtfs_block_diagnostics
{
    uint16_t api_version;
    uint16_t struct_size;
    uint64_t validity_mask;
    uint32_t reset_epoch;
    uint32_t flags;

    uint32_t capabilities;
    uint32_t status;
    uint32_t sector_size;
    uint64_t sector_count;
    uint32_t erase_block_size;
    uint32_t last_operation;
    int32_t last_error;

    uint32_t initialize_calls;
    uint32_t initialize_successes;
    uint32_t initialize_failures;
    uint32_t status_calls;
    uint32_t status_failures;
    uint32_t read_calls;
    uint32_t read_successes;
    uint32_t read_failures;
    uint32_t write_calls;
    uint32_t write_successes;
    uint32_t write_failures;
    uint32_t sync_calls;
    uint32_t sync_successes;
    uint32_t sync_failures;
    uint32_t geometry_calls;
    uint32_t geometry_failures;
    uint32_t trim_calls;
    uint32_t trim_successes;
    uint32_t trim_failures;
    uint32_t read_sectors_requested;
    uint32_t read_sectors_completed;
    uint32_t write_sectors_requested;
    uint32_t write_sectors_completed;
    uint32_t io_errors;
    uint32_t not_ready_errors;
    uint32_t no_media_errors;
    uint32_t write_protected_errors;
    uint32_t out_of_range_errors;
    uint32_t timeout_errors;
    uint32_t other_errors;
} mtfs_block_diagnostics_t;

/* Static storage embedded by ports that opt in to common diagnostics. */
/* common diagnosticsを使用するportが内部に組み込んで保持する静的storage。 */
typedef struct mtfs_block_diagnostics_state
{
    mtfs_block_diagnostics_t snapshot;
    volatile uint32_t sequence;
#if MTFS_ENABLE_STORAGE_SENTINEL
    mtfs_error_t (*lock)(void *context);
    void (*unlock)(void *context);
    void *lock_context;
    mtfs_error_t (*operation_begin)(void *context);
    void (*operation_end)(void *context);
    void *operation_context;
#endif
} mtfs_block_diagnostics_state_t;

/** @brief Attach zeroed caller-owned diagnostic state to a device. / zero初期化済みの呼び出し側所有diagnostic stateをdeviceへattachする。
 * @param device Valid device not concurrently used. / 他の処理から同時に使用されていない有効なdevice。
 * @param state Storage retained until the device is no longer used. / deviceの使用終了まで有効な状態を維持するstorage。
 * @return MTFS_OK or validation/state error. / MTFS_OKまたはvalidation/state error。
 * @pre Attach before registration or I/O. / registryへの登録またはI/O開始より前にattachすること。 */
mtfs_error_t mtfs_block_diagnostics_attach(
    mtfs_block_device_t *device, mtfs_block_diagnostics_state_t *state);

#if MTFS_ENABLE_STORAGE_SENTINEL
/** @brief Attach diagnostics with a task-context snapshot/reset lock. / task contextからのsnapshot取得／reset処理を保護するlock付きでdiagnosticsをattachする。
 * @param device Valid idle device. / 他の処理を実行していない有効なdevice。
 * @param state Caller-owned state. / 呼び出し側が所有するdiagnostic state。
 * @param lock Lock callback; paired with unlock. / unlockと対になるlock callback。
 * @param unlock Unlock callback. / unlock callback。
 * @param lock_context Opaque callback context retained by the state. / stateが参照を保持するopaque callback context。
 * @return MTFS_OK or validation/state error. / MTFS_OKまたはvalidation/state error。 */
mtfs_error_t mtfs_block_diagnostics_attach_locked(
    mtfs_block_device_t *device, mtfs_block_diagnostics_state_t *state,
    mtfs_error_t (*lock)(void *context), void (*unlock)(void *context),
    void *lock_context);
#endif

/** @brief Copy a coherent diagnostic snapshot without media I/O. / media I/Oを行わず、整合性の取れたdiagnostic snapshotをコピーする。
 * @param device Device with attached diagnostics. / diagnosticsをattach済みのdevice。
 * @param[out] snapshot Caller storage receiving version and struct_size. / api_versionとstruct_sizeを含むsnapshotの出力先として呼び出し側が用意するstorage。
 * @return MTFS_OK or an argument/state/lock error. / MTFS_OKまたは引数/state/lock error。
 * @note Task context only; counters saturate and are never wrapped. / task contextからのみ呼び出すこと。counterは最大値で飽和し、wraparoundしない。 */
mtfs_error_t mtfs_block_diagnostics_get(
    mtfs_block_device_t *device, mtfs_block_diagnostics_t *snapshot);

/** @brief Reset counters and advance reset_epoch without media I/O. / media I/Oを行わずcounterをresetし、reset_epochを更新する。
 * @param device Device with attached diagnostics. / diagnosticsをattach済みのdevice。
 * @return MTFS_OK or an argument/state/lock error. / MTFS_OKまたは引数/state/lock error。
 * @note Task context only. Existing snapshots remain values from the previous epoch. / task contextからのみ呼び出すこと。reset前に取得済みのsnapshotは、previous epochの値としてそのまま有効。 */
mtfs_error_t mtfs_block_diagnostics_reset(mtfs_block_device_t *device);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* MTFS_BLOCK_DIAGNOSTICS_H */
