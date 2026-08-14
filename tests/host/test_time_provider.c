#include "test_time_provider.h"

#include <string.h>

#include "mtfs_time.h"

typedef struct fake_time_context
{
    mtfs_datetime_t datetime;
    mtfs_time_status_t status;
    int marker;
    int fail_get;
    unsigned int locks;
} fake_time_context_t;

static mtfs_error_t fake_get(
    void *opaque, mtfs_datetime_t *datetime, mtfs_time_status_t *status)
{
    fake_time_context_t *context = (fake_time_context_t *)opaque;
    if (context->fail_get) {
        return MTFS_ERROR_IO;
    }
    *status = context->marker ? context->status : MTFS_TIME_STATUS_UNSET;
    if (*status == MTFS_TIME_STATUS_VALID) {
        *datetime = context->datetime;
    }
    return MTFS_OK;
}

static mtfs_error_t fake_set(void *opaque, const mtfs_datetime_t *datetime)
{
    fake_time_context_t *context = (fake_time_context_t *)opaque;
    context->datetime = *datetime;
    context->status = MTFS_TIME_STATUS_VALID;
    context->marker = 1;
    return MTFS_OK;
}

static mtfs_error_t fake_status(void *opaque, mtfs_time_status_t *status)
{
    fake_time_context_t *context = (fake_time_context_t *)opaque;
    *status = context->marker ? context->status : MTFS_TIME_STATUS_UNSET;
    return MTFS_OK;
}

static mtfs_error_t fake_clear(void *opaque)
{
    fake_time_context_t *context = (fake_time_context_t *)opaque;
    context->marker = 0;
    context->status = MTFS_TIME_STATUS_UNSET;
    return MTFS_OK;
}

static mtfs_error_t fake_lock(void *opaque)
{
    fake_time_context_t *context = (fake_time_context_t *)opaque;
    ++context->locks;
    return MTFS_OK;
}

static void fake_unlock(void *opaque)
{
    fake_time_context_t *context = (fake_time_context_t *)opaque;
    --context->locks;
}

static uint32_t expected_fat(
    uint16_t year, uint8_t month, uint8_t day,
    uint8_t hour, uint8_t minute, uint8_t second)
{
    return ((uint32_t)(year - 1980U) << 25) |
        ((uint32_t)month << 21) | ((uint32_t)day << 16) |
        ((uint32_t)hour << 11) | ((uint32_t)minute << 5) |
        ((uint32_t)second >> 1);
}

int test_time_provider(mtfs_test_t *test)
{
    static const mtfs_time_provider_ops_t fake_ops = {
        fake_get, fake_set, fake_status, fake_clear
    };
    static const mtfs_datetime_t valid_dates[] = {
        {1980U, 1U, 1U, 0U, 0U, 0U},
        {2000U, 2U, 29U, 23U, 59U, 59U},
        {2024U, 4U, 30U, 12U, 34U, 56U},
        {2100U, 2U, 28U, 0U, 0U, 0U},
        {2107U, 12U, 31U, 23U, 59U, 59U}
    };
    static const mtfs_datetime_t invalid_dates[] = {
        {0U, 1U, 1U, 0U, 0U, 0U},
        {2026U, 0U, 1U, 0U, 0U, 0U},
        {2026U, 13U, 1U, 0U, 0U, 0U},
        {2026U, 1U, 0U, 0U, 0U, 0U},
        {2026U, 4U, 31U, 0U, 0U, 0U},
        {2026U, 2U, 29U, 0U, 0U, 0U},
        {2100U, 2U, 29U, 0U, 0U, 0U},
        {2026U, 1U, 1U, 24U, 0U, 0U},
        {2026U, 1U, 1U, 0U, 60U, 0U},
        {2026U, 1U, 1U, 0U, 0U, 60U}
    };
    static const uint8_t month_days[] = {
        31U, 28U, 31U, 30U, 31U, 30U,
        31U, 31U, 30U, 31U, 30U, 31U
    };
    fake_time_context_t context;
    mtfs_time_provider_t provider;
    mtfs_datetime_t datetime;
    mtfs_time_status_t status;
    uint32_t packed;
    unsigned int index;

    memset(&context, 0, sizeof(context));
    memset(&provider, 0, sizeof(provider));
    provider.ops = &fake_ops;
    provider.context = &context;
    provider.lock = fake_lock;
    provider.unlock = fake_unlock;
    provider.lock_context = &context;

    if (!MTFS_TEST_CHECK(test,
            mtfs_time_get_status(&status) == MTFS_OK &&
                status == MTFS_TIME_STATUS_UNAVAILABLE,
            "unregistered provider reports UNAVAILABLE")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_time_get_local(&datetime, &status) == MTFS_ERROR_NOT_FOUND &&
                status == MTFS_TIME_STATUS_UNAVAILABLE,
            "unregistered provider cannot return a time")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_time_provider_register(&provider) == MTFS_OK,
            "register fake provider")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_time_provider_register(&provider) == MTFS_ERROR_ALREADY_EXISTS,
            "reject a second provider")) {
        return 1;
    }

    for (index = 0U; index < sizeof(valid_dates) / sizeof(valid_dates[0]); ++index) {
        if (!MTFS_TEST_CHECK(test, mtfs_datetime_is_valid(&valid_dates[index]),
                "accept valid calendar date")) {
            return 1;
        }
    }
    for (index = 0U; index < sizeof(invalid_dates) / sizeof(invalid_dates[0]); ++index) {
        if (!MTFS_TEST_CHECK(test, !mtfs_datetime_is_valid(&invalid_dates[index]),
                "reject invalid calendar date")) {
            return 1;
        }
    }
    for (index = 0U; index < sizeof(month_days) / sizeof(month_days[0]); ++index) {
        datetime = (mtfs_datetime_t){
            2026U, (uint8_t)(index + 1U), month_days[index], 0U, 0U, 0U
        };
        if (!MTFS_TEST_CHECK(test, mtfs_datetime_is_valid(&datetime),
                "accept the final day of each month")) {
            return 1;
        }
        ++datetime.day;
        if (!MTFS_TEST_CHECK(test, !mtfs_datetime_is_valid(&datetime),
                "reject the day after each month ends")) {
            return 1;
        }
    }
    datetime = (mtfs_datetime_t){2000U, 2U, 29U, 23U, 59U, 59U};
    if (!MTFS_TEST_CHECK(test,
            mtfs_datetime_to_fat(&datetime, &packed) == MTFS_OK &&
                packed == expected_fat(2000U, 2U, 29U, 23U, 59U, 59U),
            "pack a normal FAT timestamp")) {
        return 1;
    }
    datetime = (mtfs_datetime_t){1980U, 1U, 1U, 0U, 0U, 0U};
    if (!MTFS_TEST_CHECK(test,
            mtfs_datetime_to_fat(&datetime, &packed) == MTFS_OK &&
                packed == expected_fat(1980U, 1U, 1U, 0U, 0U, 0U),
            "pack FAT lower boundary")) {
        return 1;
    }
    datetime = (mtfs_datetime_t){2107U, 12U, 31U, 23U, 59U, 59U};
    if (!MTFS_TEST_CHECK(test,
            mtfs_datetime_to_fat(&datetime, &packed) == MTFS_OK &&
                packed == expected_fat(2107U, 12U, 31U, 23U, 59U, 59U),
            "pack FAT upper boundary")) {
        return 1;
    }
    datetime = (mtfs_datetime_t){1979U, 12U, 31U, 23U, 59U, 58U};
    if (!MTFS_TEST_CHECK(test,
            mtfs_datetime_to_fat(&datetime, &packed) == MTFS_ERROR_OUT_OF_RANGE,
            "reject year below FAT range")) {
        return 1;
    }
    datetime.year = 2108U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_datetime_to_fat(&datetime, &packed) == MTFS_ERROR_OUT_OF_RANGE,
            "reject year above FAT range")) {
        return 1;
    }
    datetime = (mtfs_datetime_t){2026U, 8U, 14U, 21U, 30U, 3U};
    if (!MTFS_TEST_CHECK(test,
            mtfs_datetime_to_fat(&datetime, &packed) == MTFS_OK &&
                (packed & 0x1FU) == 1U,
            "truncate FAT seconds to two-second resolution")) {
        return 1;
    }

    context.status = MTFS_TIME_STATUS_UNSET;
    context.marker = 0;
    if (!MTFS_TEST_CHECK(test,
            mtfs_time_get_status(&status) == MTFS_OK &&
                status == MTFS_TIME_STATUS_UNSET && context.locks == 0U,
            "missing marker reports UNSET under the provider lock")) {
        return 1;
    }
    datetime = (mtfs_datetime_t){2026U, 8U, 14U, 21U, 30U, 0U};
    if (!MTFS_TEST_CHECK(test,
            mtfs_time_set_local(&datetime) == MTFS_OK && context.marker,
            "setting time commits the fake marker")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_time_get_local(&datetime, &status) == MTFS_OK &&
                status == MTFS_TIME_STATUS_VALID,
            "set provider reports VALID")) {
        return 1;
    }
    context.status = MTFS_TIME_STATUS_ERROR;
    if (!MTFS_TEST_CHECK(test,
            mtfs_time_get_local(&datetime, &status) == MTFS_OK &&
                status == MTFS_TIME_STATUS_ERROR,
            "pass through ERROR status")) {
        return 1;
    }
    context.status = MTFS_TIME_STATUS_UNAVAILABLE;
    if (!MTFS_TEST_CHECK(test,
            mtfs_time_get_status(&status) == MTFS_OK &&
                status == MTFS_TIME_STATUS_UNAVAILABLE,
            "pass through provider UNAVAILABLE status")) {
        return 1;
    }
    context.status = MTFS_TIME_STATUS_VALID;
    context.fail_get = 1;
    if (!MTFS_TEST_CHECK(test,
            mtfs_time_get_local(&datetime, &status) == MTFS_ERROR_IO &&
                status == MTFS_TIME_STATUS_ERROR,
            "map provider read failure to ERROR")) {
        return 1;
    }
    context.fail_get = 0;
    if (!MTFS_TEST_CHECK(test,
            mtfs_time_clear() == MTFS_OK && !context.marker &&
                mtfs_time_get_status(&status) == MTFS_OK &&
                status == MTFS_TIME_STATUS_UNSET,
            "clear removes the setting marker")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_time_provider_unregister(&provider) == MTFS_OK,
            "unregister fake provider")) {
        return 1;
    }
    return 0;
}
