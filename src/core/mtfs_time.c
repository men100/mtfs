#include "mtfs_time.h"

#include <stddef.h>

static mtfs_time_provider_t *mtfs_time_provider;

static int mtfs_time_provider_is_valid(const mtfs_time_provider_t *provider)
{
    if ((provider == NULL) || (provider->ops == NULL) ||
        (provider->ops->get_local == NULL) ||
        (provider->ops->set_local == NULL) ||
        (provider->ops->get_status == NULL) ||
        (provider->ops->clear == NULL)) {
        return 0;
    }
    return ((provider->lock == NULL) && (provider->unlock == NULL)) ||
        ((provider->lock != NULL) && (provider->unlock != NULL));
}

static mtfs_error_t mtfs_time_lock(mtfs_time_provider_t *provider)
{
    if (provider->lock == NULL) {
        return MTFS_OK;
    }
    return provider->lock(provider->lock_context);
}

static void mtfs_time_unlock(mtfs_time_provider_t *provider)
{
    if (provider->unlock != NULL) {
        provider->unlock(provider->lock_context);
    }
}

static int mtfs_time_is_leap_year(uint16_t year)
{
    return ((year % 4U) == 0U) &&
        (((year % 100U) != 0U) || ((year % 400U) == 0U));
}

static uint8_t mtfs_time_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = {
        31U, 28U, 31U, 30U, 31U, 30U,
        31U, 31U, 30U, 31U, 30U, 31U
    };
    uint8_t result;

    if ((month < 1U) || (month > 12U)) {
        return 0U;
    }
    result = days[month - 1U];
    if ((month == 2U) && mtfs_time_is_leap_year(year)) {
        ++result;
    }
    return result;
}

mtfs_error_t mtfs_time_provider_register(mtfs_time_provider_t *provider)
{
    if (!mtfs_time_provider_is_valid(provider)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    if (mtfs_time_provider != NULL) {
        return MTFS_ERROR_ALREADY_EXISTS;
    }
    mtfs_time_provider = provider;
    return MTFS_OK;
}

mtfs_error_t mtfs_time_provider_unregister(mtfs_time_provider_t *provider)
{
    if (provider == NULL) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    if (mtfs_time_provider != provider) {
        return MTFS_ERROR_NOT_FOUND;
    }
    mtfs_time_provider = NULL;
    return MTFS_OK;
}

mtfs_error_t mtfs_time_get_local(
    mtfs_datetime_t *datetime, mtfs_time_status_t *status)
{
    mtfs_time_provider_t *provider = mtfs_time_provider;
    mtfs_error_t error;

    if ((datetime == NULL) || (status == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    *status = MTFS_TIME_STATUS_UNAVAILABLE;
    if (provider == NULL) {
        return MTFS_ERROR_NOT_FOUND;
    }
    error = mtfs_time_lock(provider);
    if (error != MTFS_OK) {
        *status = MTFS_TIME_STATUS_ERROR;
        return error;
    }
    error = provider->ops->get_local(provider->context, datetime, status);
    mtfs_time_unlock(provider);
    if (error != MTFS_OK) {
        *status = MTFS_TIME_STATUS_ERROR;
        return error;
    }
    if ((*status == MTFS_TIME_STATUS_VALID) &&
        (mtfs_datetime_to_fat(datetime, &(uint32_t){0U}) != MTFS_OK)) {
        *status = MTFS_TIME_STATUS_ERROR;
        return MTFS_ERROR_OUT_OF_RANGE;
    }
    return MTFS_OK;
}

mtfs_error_t mtfs_time_set_local(const mtfs_datetime_t *datetime)
{
    mtfs_time_provider_t *provider = mtfs_time_provider;
    mtfs_error_t error;
    uint32_t fat_timestamp;

    if (datetime == NULL) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    error = mtfs_datetime_to_fat(datetime, &fat_timestamp);
    if (error != MTFS_OK) {
        return error;
    }
    if (provider == NULL) {
        return MTFS_ERROR_NOT_SUPPORTED;
    }
    error = mtfs_time_lock(provider);
    if (error != MTFS_OK) {
        return error;
    }
    error = provider->ops->set_local(provider->context, datetime);
    mtfs_time_unlock(provider);
    return error;
}

mtfs_error_t mtfs_time_get_status(mtfs_time_status_t *status)
{
    mtfs_time_provider_t *provider = mtfs_time_provider;
    mtfs_error_t error;

    if (status == NULL) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    if (provider == NULL) {
        *status = MTFS_TIME_STATUS_UNAVAILABLE;
        return MTFS_OK;
    }
    error = mtfs_time_lock(provider);
    if (error != MTFS_OK) {
        *status = MTFS_TIME_STATUS_ERROR;
        return error;
    }
    error = provider->ops->get_status(provider->context, status);
    mtfs_time_unlock(provider);
    if (error != MTFS_OK) {
        *status = MTFS_TIME_STATUS_ERROR;
    }
    return error;
}

mtfs_error_t mtfs_time_clear(void)
{
    mtfs_time_provider_t *provider = mtfs_time_provider;
    mtfs_error_t error;

    if (provider == NULL) {
        return MTFS_ERROR_NOT_SUPPORTED;
    }
    error = mtfs_time_lock(provider);
    if (error != MTFS_OK) {
        return error;
    }
    error = provider->ops->clear(provider->context);
    mtfs_time_unlock(provider);
    return error;
}

int mtfs_datetime_is_valid(const mtfs_datetime_t *datetime)
{
    uint8_t days;

    if ((datetime == NULL) || (datetime->year == 0U) ||
        (datetime->month < 1U) || (datetime->month > 12U) ||
        (datetime->hour > 23U) || (datetime->minute > 59U) ||
        (datetime->second > 59U)) {
        return 0;
    }
    days = mtfs_time_days_in_month(datetime->year, datetime->month);
    return (datetime->day >= 1U) && (datetime->day <= days);
}

mtfs_error_t mtfs_datetime_to_fat(
    const mtfs_datetime_t *datetime, uint32_t *fat_timestamp)
{
    if ((datetime == NULL) || (fat_timestamp == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    *fat_timestamp = 0U;
    if (!mtfs_datetime_is_valid(datetime)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    if ((datetime->year < 1980U) || (datetime->year > 2107U)) {
        return MTFS_ERROR_OUT_OF_RANGE;
    }
    *fat_timestamp = ((uint32_t)(datetime->year - 1980U) << 25) |
        ((uint32_t)datetime->month << 21) |
        ((uint32_t)datetime->day << 16) |
        ((uint32_t)datetime->hour << 11) |
        ((uint32_t)datetime->minute << 5) |
        ((uint32_t)datetime->second >> 1);
    return MTFS_OK;
}
