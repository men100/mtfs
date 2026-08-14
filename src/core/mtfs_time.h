/* Platform-independent local calendar time provider for microT-FS. */
#ifndef MTFS_TIME_H
#define MTFS_TIME_H

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

mtfs_error_t mtfs_time_provider_register(mtfs_time_provider_t *provider);
mtfs_error_t mtfs_time_provider_unregister(mtfs_time_provider_t *provider);
mtfs_error_t mtfs_time_get_local(
    mtfs_datetime_t *datetime, mtfs_time_status_t *status);
mtfs_error_t mtfs_time_set_local(const mtfs_datetime_t *datetime);
mtfs_error_t mtfs_time_get_status(mtfs_time_status_t *status);
mtfs_error_t mtfs_time_clear(void);

int mtfs_datetime_is_valid(const mtfs_datetime_t *datetime);
mtfs_error_t mtfs_datetime_to_fat(
    const mtfs_datetime_t *datetime, uint32_t *fat_timestamp);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_TIME_H */
