#ifndef MTFS_FF_FS_NORTC
#define MTFS_FF_FS_NORTC (1)
#endif

#if !MTFS_FF_FS_NORTC

#include "mtfs_stm32_rtc.h"

#include <stddef.h>
#include <string.h>

#define MTFS_STM32_RTC_MARKER_MAGIC        UINT32_C(0x4D544653)
#define MTFS_STM32_RTC_MARKER_VERSION      UINT32_C(0x00010001)
#define MTFS_STM32_RTC_LSI_TIMEOUT_MS      UINT32_C(1000)

static int mtfs_stm32_rtc_marker_is_valid(
    const mtfs_stm32_rtc_context_t *context)
{
    uint32_t magic = HAL_RTCEx_BKUPRead(
        &context->rtc, MTFS_STM32_RTC_MARKER_REGISTER_MAGIC);
    uint32_t version = HAL_RTCEx_BKUPRead(
        &context->rtc, MTFS_STM32_RTC_MARKER_REGISTER_VERSION);
    uint32_t inverse = HAL_RTCEx_BKUPRead(
        &context->rtc, MTFS_STM32_RTC_MARKER_REGISTER_INVERSE);

    return (magic == MTFS_STM32_RTC_MARKER_MAGIC) &&
        (version == MTFS_STM32_RTC_MARKER_VERSION) &&
        (inverse == ~MTFS_STM32_RTC_MARKER_MAGIC);
}

static void mtfs_stm32_rtc_marker_clear(
    const mtfs_stm32_rtc_context_t *context)
{
    HAL_RTCEx_BKUPWrite(
        &context->rtc, MTFS_STM32_RTC_MARKER_REGISTER_MAGIC, 0U);
    __DSB();
    HAL_RTCEx_BKUPWrite(
        &context->rtc, MTFS_STM32_RTC_MARKER_REGISTER_VERSION, 0U);
    HAL_RTCEx_BKUPWrite(
        &context->rtc, MTFS_STM32_RTC_MARKER_REGISTER_INVERSE, 0U);
    __DSB();
}

static int mtfs_stm32_rtc_marker_commit(
    const mtfs_stm32_rtc_context_t *context)
{
    HAL_RTCEx_BKUPWrite(&context->rtc,
        MTFS_STM32_RTC_MARKER_REGISTER_VERSION,
        MTFS_STM32_RTC_MARKER_VERSION);
    HAL_RTCEx_BKUPWrite(&context->rtc,
        MTFS_STM32_RTC_MARKER_REGISTER_INVERSE,
        ~MTFS_STM32_RTC_MARKER_MAGIC);
    __DSB();
    HAL_RTCEx_BKUPWrite(&context->rtc,
        MTFS_STM32_RTC_MARKER_REGISTER_MAGIC,
        MTFS_STM32_RTC_MARKER_MAGIC);
    __DSB();
    return mtfs_stm32_rtc_marker_is_valid(context);
}

static mtfs_error_t mtfs_stm32_rtc_read_calendar(
    mtfs_stm32_rtc_context_t *context, mtfs_datetime_t *datetime)
{
    RTC_TimeTypeDef time;
    RTC_DateTypeDef date;

    memset(&time, 0, sizeof(time));
    memset(&date, 0, sizeof(date));
    if (HAL_RTC_GetTime(&context->rtc, &time, RTC_FORMAT_BIN) != HAL_OK) {
        return MTFS_ERROR_IO;
    }
    /* Reading the date after the time releases the RTC shadow-register lock. */
    if (HAL_RTC_GetDate(&context->rtc, &date, RTC_FORMAT_BIN) != HAL_OK) {
        return MTFS_ERROR_IO;
    }
    datetime->year = (uint16_t)(2000U + date.Year);
    datetime->month = date.Month;
    datetime->day = date.Date;
    datetime->hour = time.Hours;
    datetime->minute = time.Minutes;
    datetime->second = time.Seconds;
    return mtfs_datetime_is_valid(datetime) ? MTFS_OK : MTFS_ERROR_IO;
}

static int mtfs_stm32_rtc_same_datetime(
    const mtfs_datetime_t *left, const mtfs_datetime_t *right)
{
    return (left->year == right->year) && (left->month == right->month) &&
        (left->day == right->day) && (left->hour == right->hour) &&
        (left->minute == right->minute) && (left->second == right->second);
}

static int mtfs_stm32_rtc_leap_year(uint16_t year)
{
    return ((year % 4U) == 0U) &&
        (((year % 100U) != 0U) || ((year % 400U) == 0U));
}

static uint8_t mtfs_stm32_rtc_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = {
        31U, 28U, 31U, 30U, 31U, 30U,
        31U, 31U, 30U, 31U, 30U, 31U
    };
    uint8_t result = days[month - 1U];
    if ((month == 2U) && mtfs_stm32_rtc_leap_year(year)) {
        ++result;
    }
    return result;
}

static int mtfs_stm32_rtc_readback_is_acceptable(
    const mtfs_datetime_t *requested, const mtfs_datetime_t *readback)
{
    mtfs_datetime_t next = *requested;

    if (mtfs_stm32_rtc_same_datetime(requested, readback)) {
        return 1;
    }
    if (++next.second >= 60U) {
        next.second = 0U;
        if (++next.minute >= 60U) {
            next.minute = 0U;
            if (++next.hour >= 24U) {
                next.hour = 0U;
                if (++next.day >
                    mtfs_stm32_rtc_days_in_month(next.year, next.month)) {
                    next.day = 1U;
                    if (++next.month > 12U) {
                        next.month = 1U;
                        ++next.year;
                    }
                }
            }
        }
    }
    return (next.year <= 2099U) &&
        mtfs_stm32_rtc_same_datetime(&next, readback);
}

static uint8_t mtfs_stm32_rtc_weekday(const mtfs_datetime_t *datetime)
{
    static const uint8_t offsets[] = {
        0U, 3U, 2U, 5U, 0U, 3U, 5U, 1U, 4U, 6U, 2U, 4U
    };
    unsigned int year = datetime->year;
    unsigned int sunday_based;

    if (datetime->month < 3U) {
        --year;
    }
    sunday_based = (year + year / 4U - year / 100U + year / 400U +
        offsets[datetime->month - 1U] + datetime->day) % 7U;
    return sunday_based == 0U ? RTC_WEEKDAY_SUNDAY : (uint8_t)sunday_based;
}

static mtfs_error_t mtfs_stm32_rtc_get(
    void *opaque, mtfs_datetime_t *datetime, mtfs_time_status_t *status)
{
    mtfs_stm32_rtc_context_t *context =
        (mtfs_stm32_rtc_context_t *)opaque;
    mtfs_error_t error;
    uint32_t fat_timestamp;

    if (!context->initialized) {
        *status = MTFS_TIME_STATUS_UNAVAILABLE;
        return MTFS_OK;
    }
    if (!mtfs_stm32_rtc_marker_is_valid(context)) {
        *status = MTFS_TIME_STATUS_UNSET;
        return MTFS_OK;
    }
    error = mtfs_stm32_rtc_read_calendar(context, datetime);
    if ((error != MTFS_OK) ||
        (mtfs_datetime_to_fat(datetime, &fat_timestamp) != MTFS_OK)) {
        *status = MTFS_TIME_STATUS_ERROR;
        return error == MTFS_OK ? MTFS_ERROR_OUT_OF_RANGE : error;
    }
    *status = MTFS_TIME_STATUS_VALID;
    return MTFS_OK;
}

static mtfs_error_t mtfs_stm32_rtc_set(
    void *opaque, const mtfs_datetime_t *datetime)
{
    mtfs_stm32_rtc_context_t *context =
        (mtfs_stm32_rtc_context_t *)opaque;
    RTC_TimeTypeDef time;
    RTC_DateTypeDef date;
    mtfs_datetime_t readback;

    if (!context->initialized) {
        return MTFS_ERROR_NOT_READY;
    }
    /* STM32 calendar mode stores a two-digit year with the 2000 epoch. */
    if ((datetime->year < 2000U) || (datetime->year > 2099U)) {
        return MTFS_ERROR_OUT_OF_RANGE;
    }
    mtfs_stm32_rtc_marker_clear(context);
    memset(&time, 0, sizeof(time));
    memset(&date, 0, sizeof(date));
    time.Hours = datetime->hour;
    time.Minutes = datetime->minute;
    time.Seconds = datetime->second;
    time.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    time.StoreOperation = RTC_STOREOPERATION_RESET;
    date.Year = (uint8_t)(datetime->year - 2000U);
    date.Month = datetime->month;
    date.Date = datetime->day;
    date.WeekDay = mtfs_stm32_rtc_weekday(datetime);

    if ((HAL_RTC_SetTime(&context->rtc, &time, RTC_FORMAT_BIN) != HAL_OK) ||
        (HAL_RTC_SetDate(&context->rtc, &date, RTC_FORMAT_BIN) != HAL_OK) ||
        (mtfs_stm32_rtc_read_calendar(context, &readback) != MTFS_OK) ||
        !mtfs_stm32_rtc_readback_is_acceptable(datetime, &readback)) {
        mtfs_stm32_rtc_marker_clear(context);
        return MTFS_ERROR_IO;
    }
    if (!mtfs_stm32_rtc_marker_commit(context)) {
        mtfs_stm32_rtc_marker_clear(context);
        return MTFS_ERROR_IO;
    }
    return MTFS_OK;
}

static mtfs_error_t mtfs_stm32_rtc_status(
    void *opaque, mtfs_time_status_t *status)
{
    mtfs_datetime_t datetime;
    return mtfs_stm32_rtc_get(opaque, &datetime, status);
}

static mtfs_error_t mtfs_stm32_rtc_clear(void *opaque)
{
    mtfs_stm32_rtc_context_t *context =
        (mtfs_stm32_rtc_context_t *)opaque;
    if (!context->initialized) {
        return MTFS_ERROR_NOT_READY;
    }
    mtfs_stm32_rtc_marker_clear(context);
    return mtfs_stm32_rtc_marker_is_valid(context) ? MTFS_ERROR_IO : MTFS_OK;
}

static mtfs_error_t mtfs_stm32_rtc_clock_init(void)
{
    RCC_PeriphCLKInitTypeDef clock;
    uint32_t source;
    uint32_t started;

    HAL_PWR_EnableBkUpAccess();
    source = __HAL_RCC_GET_RTC_SOURCE();
    if ((source != RCC_RTCCLKSOURCE_DISABLE) &&
        (source != RCC_RTCCLKSOURCE_LSI)) {
        /* Never reset a live backup domain merely to change its clock source. */
        return MTFS_ERROR_NOT_READY;
    }
    __HAL_RCC_LSI_ENABLE();
    started = HAL_GetTick();
    while (__HAL_RCC_GET_FLAG(RCC_FLAG_LSIRDY) == 0U) {
        if ((HAL_GetTick() - started) >= MTFS_STM32_RTC_LSI_TIMEOUT_MS) {
            return MTFS_ERROR_NOT_READY;
        }
    }
    if (source == RCC_RTCCLKSOURCE_DISABLE) {
        memset(&clock, 0, sizeof(clock));
        clock.PeriphClockSelection = RCC_PERIPHCLK_RTC;
        clock.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;
        if (HAL_RCCEx_PeriphCLKConfig(&clock) != HAL_OK) {
            return MTFS_ERROR_IO;
        }
    }
    __HAL_RCC_RTCAPB_CLK_ENABLE();
    __HAL_RCC_RTC_CLK_ENABLE();
    __HAL_RCC_RTCAPB_CLK_SLEEP_ENABLE();
    __HAL_RCC_RTC_CLK_SLEEP_ENABLE();
    return MTFS_OK;
}

mtfs_error_t mtfs_stm32_rtc_init(
    mtfs_stm32_rtc_context_t *context,
    mtfs_error_t (*lock)(void *lock_context),
    void (*unlock)(void *lock_context),
    void *lock_context)
{
    static const mtfs_time_provider_ops_t ops = {
        mtfs_stm32_rtc_get,
        mtfs_stm32_rtc_set,
        mtfs_stm32_rtc_status,
        mtfs_stm32_rtc_clear
    };
    mtfs_error_t error;

    if ((context == NULL) || ((lock == NULL) != (unlock == NULL))) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    memset(context, 0, sizeof(*context));
    context->reset_flags_at_init = RCC->RSR;
    error = mtfs_stm32_rtc_clock_init();
    if (error != MTFS_OK) {
        return error;
    }
    context->rtc.Instance = RTC;
    context->rtc.Init.HourFormat = RTC_HOURFORMAT_24;
    context->rtc.Init.AsynchPrediv = 127U;
    context->rtc.Init.SynchPrediv = 249U;
    context->rtc.Init.OutPut = RTC_OUTPUT_DISABLE;
    context->rtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
    context->rtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
    context->rtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
    context->rtc.Init.OutPutPullUp = RTC_OUTPUT_PULLUP_NONE;
    context->rtc.Init.BinMode = RTC_BINARY_NONE;
    context->rtc.Init.BinMixBcdU = RTC_BINARY_MIX_BCDU_0;
    if (HAL_RTC_Init(&context->rtc) != HAL_OK) {
        return MTFS_ERROR_IO;
    }
    context->provider.ops = &ops;
    context->provider.context = context;
    context->provider.lock = lock;
    context->provider.unlock = unlock;
    context->provider.lock_context = lock_context;
    context->initialized = 1;
    return MTFS_OK;
}

mtfs_time_provider_t *mtfs_stm32_rtc_provider(
    mtfs_stm32_rtc_context_t *context)
{
    if ((context == NULL) || !context->initialized) {
        return NULL;
    }
    return &context->provider;
}

#else

typedef int mtfs_stm32_rtc_provider_not_selected_t;

#endif
