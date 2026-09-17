/** @file mtfs_time.h
 * @brief Local calendar time provider and FAT conversion. / local calendar time providerとFAT timestamp変換。
 * @ingroup mtfs_time */
#ifndef MTFS_TIME_H
#define MTFS_TIME_H

/** @addtogroup mtfs_time
 * @{ */

#include <stdint.h>

#include "../mtfs_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mtfs_time_status
{
    MTFS_TIME_STATUS_VALID = 0,
    MTFS_TIME_STATUS_UNSET,
    MTFS_TIME_STATUS_ERROR,
    MTFS_TIME_STATUS_UNAVAILABLE
} mtfs_time_status_t;

typedef struct mtfs_datetime
{
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} mtfs_datetime_t;

typedef struct mtfs_time_provider_ops
{
    mtfs_error_t (*get_local)(
        void *context, mtfs_datetime_t *datetime, mtfs_time_status_t *status);
    mtfs_error_t (*set_local)(void *context, const mtfs_datetime_t *datetime);
    mtfs_error_t (*get_status)(void *context, mtfs_time_status_t *status);
    mtfs_error_t (*clear)(void *context);
} mtfs_time_provider_ops_t;

typedef struct mtfs_time_provider
{
    const mtfs_time_provider_ops_t *ops;
    void *context;
    mtfs_error_t (*lock)(void *lock_context);
    void (*unlock)(void *lock_context);
    void *lock_context;
} mtfs_time_provider_t;

/** @brief Register the singleton provider without taking ownership. / providerの所有権を取得せず、singletonのtime providerとして登録する。
 * @param provider Complete operation table; lock/unlock must be both NULL or both set. / 必要なoperationをすべて設定したprovider。lockとunlockは両方NULL、または両方設定すること。
 * @return MTFS_OK, INVALID_ARGUMENT, or ALREADY_EXISTS. / MTFS_OK、INVALID_ARGUMENT、またはALREADY_EXISTS。
 * @note Keep provider and context alive until unregister. / unregisterするまでproviderとcontextを有効な状態に維持すること。 */
mtfs_error_t mtfs_time_provider_register(mtfs_time_provider_t *provider);

/** @brief Unregister the exact current provider. / 現在登録されているproviderと同一のinstanceをunregisterする。
 * @param provider Provider previously registered. / 事前に登録したprovider。
 * @return MTFS_OK, INVALID_ARGUMENT, or NOT_FOUND. / MTFS_OK、INVALID_ARGUMENT、またはNOT_FOUND。 */
mtfs_error_t mtfs_time_provider_unregister(mtfs_time_provider_t *provider);

/** @brief Read validated local calendar time. / 検証済みの local calendar timeを取得する。
 * @param[out] datetime Calendar fields when available. / 時刻を取得できた場合のcalendar値。
 * @param[out] status VALID, UNSET, ERROR, or UNAVAILABLE. / time providerの状態。VALID、UNSET、ERROR、またはUNAVAILABLE。
 * @return MTFS_OK or provider/validation/lock error. / MTFS_OKまたはprovider/validation/lock error。 */
mtfs_error_t mtfs_time_get_local(
    mtfs_datetime_t *datetime, mtfs_time_status_t *status);

/** @brief Set validated local calendar time. / local calendar timeを検証して設定する。
 * @param datetime Calendar value representable by FAT (1980..2107). / FAT timestampで表現可能なcalendar値（1980～2107年）。
 * @return MTFS_OK or validation/provider/lock error. / MTFS_OKまたはvalidation/provider/lock error。 */
mtfs_error_t mtfs_time_set_local(const mtfs_datetime_t *datetime);

/** @brief Query provider status without reading time fields. / calendar timeを取得せず、providerのstatusだけを取得する。
 * @param[out] status UNAVAILABLE when no provider is registered. / providerが登録されていない場合はUNAVAILABLE。
 * @return MTFS_OK or provider/lock error. / MTFS_OKまたはprovider/lock error。 */
mtfs_error_t mtfs_time_get_status(mtfs_time_status_t *status);

/** @brief Clear the provider's stored time. / providerに保存されている時刻情報を消去する。
 * @return MTFS_OK, NOT_SUPPORTED, or provider/lock error. / MTFS_OK、NOT_SUPPORTED、またはprovider/lock error。 */
mtfs_error_t mtfs_time_clear(void);

/** @brief Validate Gregorian calendar fields. / Gregorian calendarとして各fieldの値が有効か検証する。
 * @param datetime Value to validate. / 検証する日時。
 * @return Nonzero if valid; year need not be FAT-representable. / 有効な場合は非0。yearはFAT timestampで表現可能な範囲でなくてもよい。 */
int mtfs_datetime_is_valid(const mtfs_datetime_t *datetime);

/** @brief Encode FAT local timestamp (two-second resolution). / local calendar timeを2秒単位のFAT timestampへencodeする。
 * @param datetime Valid year 1980..2107 calendar value. / 1980～2107年の範囲にある有効なcalendar値。
 * @param[out] fat_timestamp Packed timestamp. / encodeしたpacked FAT timestampの出力先。
 * @return MTFS_OK, INVALID_ARGUMENT, or OUT_OF_RANGE. / MTFS_OK、INVALID_ARGUMENT、またはOUT_OF_RANGE。 */
mtfs_error_t mtfs_datetime_to_fat(
    const mtfs_datetime_t *datetime, uint32_t *fat_timestamp);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* MTFS_TIME_H */
