#ifndef MTFS_FF_FS_NORTC
#define MTFS_FF_FS_NORTC (1)
#endif

#if !MTFS_FF_FS_NORTC

#include "mtfs_ra_rtc.h"

#include <stddef.h>
#include <string.h>

#include "bsp_api.h"

#define MTFS_RA_RTC_MARKER_MAGIC   UINT32_C(0x4D544653)
#define MTFS_RA_RTC_MARKER_VERSION UINT32_C(0x00010001)
#define MTFS_RA_RTC_VBPORF_MASK    UINT8_C(0x01)
#define MTFS_RA_RTC_VBAE_MASK      UINT8_C(0x08)

static int mtfs_ra_rtc_backup_access_begin(void)
{
    R_BSP_RegisterProtectDisable(BSP_REG_PROTECT_OM_LPC_BATT);
    R_SYSTEM->VBTBER = MTFS_RA_RTC_VBAE_MASK;
    __DSB();
    if ((R_SYSTEM->VBTBER & MTFS_RA_RTC_VBAE_MASK) == 0U) {
        R_BSP_RegisterProtectEnable(BSP_REG_PROTECT_OM_LPC_BATT);
        return 0;
    }
    /* RA8P1 requires at least 500 ns before the first VBTBKR access. */
    R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MICROSECONDS);
    return 1;
}

static void mtfs_ra_rtc_backup_access_end(void)
{
    __DSB();
    R_SYSTEM->VBTBER = 0U;
    __DSB();
    R_BSP_RegisterProtectEnable(BSP_REG_PROTECT_OM_LPC_BATT);
}

static int mtfs_ra_rtc_backup_power_loss_clear(void)
{
    int cleared;

    R_BSP_RegisterProtectDisable(BSP_REG_PROTECT_OM_LPC_BATT);
    R_SYSTEM->VBTBPSR = 0U;
    __DSB();
    cleared = (R_SYSTEM->VBTBPSR & MTFS_RA_RTC_VBPORF_MASK) == 0U;
    R_BSP_RegisterProtectEnable(BSP_REG_PROTECT_OM_LPC_BATT);
    return cleared;
}

static uint32_t mtfs_ra_rtc_marker_read_word(unsigned int offset)
{
    uint32_t value = 0U;
    unsigned int byte;

    for (byte = 0U; byte < 4U; ++byte) {
        value |= (uint32_t)R_SYSTEM->VBTBKR[offset + byte] << (byte * 8U);
    }
    return value;
}

static void mtfs_ra_rtc_marker_write_word(
    unsigned int offset, uint32_t value)
{
    unsigned int byte;

    for (byte = 0U; byte < 4U; ++byte) {
        R_SYSTEM->VBTBKR[offset + byte] =
            (uint8_t)(value >> (byte * 8U));
    }
}

static int mtfs_ra_rtc_marker_is_valid(void)
{
    uint32_t magic;
    uint32_t version;
    uint32_t inverse;

    if (!mtfs_ra_rtc_backup_access_begin()) {
        return 0;
    }
    magic = mtfs_ra_rtc_marker_read_word(MTFS_RA_RTC_MARKER_OFFSET_MAGIC);
    version = mtfs_ra_rtc_marker_read_word(
        MTFS_RA_RTC_MARKER_OFFSET_VERSION);
    inverse = mtfs_ra_rtc_marker_read_word(
        MTFS_RA_RTC_MARKER_OFFSET_INVERSE);
    mtfs_ra_rtc_backup_access_end();
    return (magic == MTFS_RA_RTC_MARKER_MAGIC) &&
        (version == MTFS_RA_RTC_MARKER_VERSION) &&
        (inverse == ~MTFS_RA_RTC_MARKER_MAGIC);
}

static int mtfs_ra_rtc_marker_clear(void)
{
    if (!mtfs_ra_rtc_backup_access_begin()) {
        return 0;
    }
    /* Invalidate the marker before changing the remaining fields. */
    mtfs_ra_rtc_marker_write_word(MTFS_RA_RTC_MARKER_OFFSET_MAGIC, 0U);
    __DSB();
    mtfs_ra_rtc_marker_write_word(MTFS_RA_RTC_MARKER_OFFSET_VERSION, 0U);
    mtfs_ra_rtc_marker_write_word(MTFS_RA_RTC_MARKER_OFFSET_INVERSE, 0U);
    __DSB();
    mtfs_ra_rtc_backup_access_end();
    return !mtfs_ra_rtc_marker_is_valid();
}

static int mtfs_ra_rtc_marker_commit(void)
{
    if (!mtfs_ra_rtc_backup_access_begin()) {
        return 0;
    }
    mtfs_ra_rtc_marker_write_word(MTFS_RA_RTC_MARKER_OFFSET_VERSION,
        MTFS_RA_RTC_MARKER_VERSION);
    mtfs_ra_rtc_marker_write_word(MTFS_RA_RTC_MARKER_OFFSET_INVERSE,
        ~MTFS_RA_RTC_MARKER_MAGIC);
    __DSB();
    /* Magic is the commit record and must be written last. */
    mtfs_ra_rtc_marker_write_word(MTFS_RA_RTC_MARKER_OFFSET_MAGIC,
        MTFS_RA_RTC_MARKER_MAGIC);
    __DSB();
    mtfs_ra_rtc_backup_access_end();
    return mtfs_ra_rtc_marker_is_valid();
}

static mtfs_error_t mtfs_ra_rtc_record_fsp_error(
    mtfs_ra_rtc_context_t *context, fsp_err_t error)
{
    context->last_fsp_error = error;
    return error == FSP_SUCCESS ? MTFS_OK : MTFS_ERROR_IO;
}

static mtfs_error_t mtfs_ra_rtc_read_calendar(
    mtfs_ra_rtc_context_t *context, mtfs_datetime_t *datetime)
{
    rtc_time_t time;
    fsp_err_t error;

    memset(&time, 0, sizeof(time));
    error = context->rtc->p_api->calendarTimeGet(
        context->rtc->p_ctrl, &time);
    if (error != FSP_SUCCESS) {
        return mtfs_ra_rtc_record_fsp_error(context, error);
    }
    context->last_fsp_error = FSP_SUCCESS;
    if ((time.tm_year < 100) || (time.tm_year > 199) ||
        (time.tm_mon < 0) || (time.tm_mon > 11) ||
        (time.tm_mday < 1) || (time.tm_mday > 31) ||
        (time.tm_hour < 0) || (time.tm_hour > 23) ||
        (time.tm_min < 0) || (time.tm_min > 59) ||
        (time.tm_sec < 0) || (time.tm_sec > 59)) {
        return MTFS_ERROR_IO;
    }
    datetime->year = (uint16_t)(time.tm_year + 1900);
    datetime->month = (uint8_t)(time.tm_mon + 1);
    datetime->day = (uint8_t)time.tm_mday;
    datetime->hour = (uint8_t)time.tm_hour;
    datetime->minute = (uint8_t)time.tm_min;
    datetime->second = (uint8_t)time.tm_sec;
    return mtfs_datetime_is_valid(datetime) ? MTFS_OK : MTFS_ERROR_IO;
}

static int mtfs_ra_rtc_same_datetime(
    const mtfs_datetime_t *left, const mtfs_datetime_t *right)
{
    return (left->year == right->year) && (left->month == right->month) &&
        (left->day == right->day) && (left->hour == right->hour) &&
        (left->minute == right->minute) && (left->second == right->second);
}

static int mtfs_ra_rtc_leap_year(uint16_t year)
{
    return ((year % 4U) == 0U) &&
        (((year % 100U) != 0U) || ((year % 400U) == 0U));
}

static uint8_t mtfs_ra_rtc_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = {
        31U, 28U, 31U, 30U, 31U, 30U,
        31U, 31U, 30U, 31U, 30U, 31U
    };
    uint8_t result = days[month - 1U];

    if ((month == 2U) && mtfs_ra_rtc_leap_year(year)) {
        ++result;
    }
    return result;
}

static int mtfs_ra_rtc_readback_is_acceptable(
    const mtfs_datetime_t *requested, const mtfs_datetime_t *readback)
{
    mtfs_datetime_t next = *requested;

    if (mtfs_ra_rtc_same_datetime(requested, readback)) {
        return 1;
    }
    if (++next.second >= 60U) {
        next.second = 0U;
        if (++next.minute >= 60U) {
            next.minute = 0U;
            if (++next.hour >= 24U) {
                next.hour = 0U;
                if (++next.day >
                    mtfs_ra_rtc_days_in_month(next.year, next.month)) {
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
        mtfs_ra_rtc_same_datetime(&next, readback);
}

static int mtfs_ra_rtc_weekday(const mtfs_datetime_t *datetime)
{
    static const uint8_t offsets[] = {
        0U, 3U, 2U, 5U, 0U, 3U, 5U, 1U, 4U, 6U, 2U, 4U
    };
    unsigned int year = datetime->year;

    if (datetime->month < 3U) {
        --year;
    }
    return (int)((year + year / 4U - year / 100U + year / 400U +
        offsets[datetime->month - 1U] + datetime->day) % 7U);
}

static mtfs_error_t mtfs_ra_rtc_get(
    void *opaque, mtfs_datetime_t *datetime, mtfs_time_status_t *status)
{
    mtfs_ra_rtc_context_t *context = (mtfs_ra_rtc_context_t *)opaque;
    mtfs_error_t error;
    uint32_t fat_timestamp;

    if (!context->initialized) {
        *status = MTFS_TIME_STATUS_UNAVAILABLE;
        return MTFS_OK;
    }
    if (!mtfs_ra_rtc_marker_is_valid()) {
        *status = MTFS_TIME_STATUS_UNSET;
        return MTFS_OK;
    }
    error = mtfs_ra_rtc_read_calendar(context, datetime);
    if ((error != MTFS_OK) ||
        (mtfs_datetime_to_fat(datetime, &fat_timestamp) != MTFS_OK)) {
        *status = MTFS_TIME_STATUS_ERROR;
        return error == MTFS_OK ? MTFS_ERROR_OUT_OF_RANGE : error;
    }
    *status = MTFS_TIME_STATUS_VALID;
    return MTFS_OK;
}

static mtfs_error_t mtfs_ra_rtc_set(
    void *opaque, const mtfs_datetime_t *datetime)
{
    mtfs_ra_rtc_context_t *context = (mtfs_ra_rtc_context_t *)opaque;
    rtc_time_t time;
    mtfs_datetime_t readback;
    fsp_err_t fsp_error;

    if (!context->initialized) {
        return MTFS_ERROR_NOT_READY;
    }
    /* RA calendar mode stores a two-digit year with the 2000 epoch. */
    if ((datetime->year < 2000U) || (datetime->year > 2099U)) {
        return MTFS_ERROR_OUT_OF_RANGE;
    }
    if (!mtfs_ra_rtc_marker_clear()) {
        return MTFS_ERROR_IO;
    }
    memset(&time, 0, sizeof(time));
    time.tm_year = (int)datetime->year - 1900;
    time.tm_mon = (int)datetime->month - 1;
    time.tm_mday = datetime->day;
    time.tm_hour = datetime->hour;
    time.tm_min = datetime->minute;
    time.tm_sec = datetime->second;
    time.tm_wday = mtfs_ra_rtc_weekday(datetime);

    fsp_error = context->rtc->p_api->calendarTimeSet(
        context->rtc->p_ctrl, &time);
    if (fsp_error != FSP_SUCCESS) {
        (void)mtfs_ra_rtc_record_fsp_error(context, fsp_error);
        return MTFS_ERROR_IO;
    }
    context->last_fsp_error = FSP_SUCCESS;
    if ((mtfs_ra_rtc_read_calendar(context, &readback) != MTFS_OK) ||
        !mtfs_ra_rtc_readback_is_acceptable(datetime, &readback) ||
        !mtfs_ra_rtc_marker_commit()) {
        (void)mtfs_ra_rtc_marker_clear();
        return MTFS_ERROR_IO;
    }
    return MTFS_OK;
}

static mtfs_error_t mtfs_ra_rtc_status(
    void *opaque, mtfs_time_status_t *status)
{
    mtfs_datetime_t datetime;
    return mtfs_ra_rtc_get(opaque, &datetime, status);
}

static mtfs_error_t mtfs_ra_rtc_clear(void *opaque)
{
    mtfs_ra_rtc_context_t *context = (mtfs_ra_rtc_context_t *)opaque;

    if (!context->initialized) {
        return MTFS_ERROR_NOT_READY;
    }
    return mtfs_ra_rtc_marker_clear() ? MTFS_OK : MTFS_ERROR_IO;
}

mtfs_error_t mtfs_ra_rtc_init(
    mtfs_ra_rtc_context_t *context,
    rtc_instance_t const *rtc,
    mtfs_error_t (*lock)(void *lock_context),
    void (*unlock)(void *lock_context),
    void *lock_context)
{
    static const mtfs_time_provider_ops_t ops = {
        mtfs_ra_rtc_get,
        mtfs_ra_rtc_set,
        mtfs_ra_rtc_status,
        mtfs_ra_rtc_clear
    };
    int marker_valid;
    fsp_err_t fsp_error;

    if ((context == NULL) || (rtc == NULL) || (rtc->p_api == NULL) ||
        (rtc->p_ctrl == NULL) || (rtc->p_cfg == NULL) ||
        ((lock == NULL) != (unlock == NULL))) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    memset(context, 0, sizeof(*context));
    context->rtc = rtc;
    context->vbatt_status_at_init = R_SYSTEM->VBTBPSR;
    context->backup_power_loss_at_init =
        (context->vbatt_status_at_init & MTFS_RA_RTC_VBPORF_MASK) != 0U;
    marker_valid = !context->backup_power_loss_at_init &&
        mtfs_ra_rtc_marker_is_valid();

    fsp_error = rtc->p_api->open(rtc->p_ctrl, rtc->p_cfg);
    if (fsp_error != FSP_SUCCESS) {
        return mtfs_ra_rtc_record_fsp_error(context, fsp_error);
    }
    if (!marker_valid) {
        if (!mtfs_ra_rtc_marker_clear()) {
            (void)rtc->p_api->close(rtc->p_ctrl);
            return MTFS_ERROR_IO;
        }
    }
    if (context->backup_power_loss_at_init) {
        /* FSP requires source selection only once after backup-domain loss. */
        fsp_error = rtc->p_api->clockSourceSet(rtc->p_ctrl);
        if (fsp_error != FSP_SUCCESS) {
            (void)rtc->p_api->close(rtc->p_ctrl);
            return mtfs_ra_rtc_record_fsp_error(context, fsp_error);
        }
        context->clock_source_initialized = 1;
        /* Clear VBPORF only after source initialization has succeeded. */
        if (!mtfs_ra_rtc_backup_power_loss_clear()) {
            (void)rtc->p_api->close(rtc->p_ctrl);
            return MTFS_ERROR_IO;
        }
    }
    context->last_fsp_error = FSP_SUCCESS;
    context->provider.ops = &ops;
    context->provider.context = context;
    context->provider.lock = lock;
    context->provider.unlock = unlock;
    context->provider.lock_context = lock_context;
    context->initialized = 1;
    return MTFS_OK;
}

mtfs_time_provider_t *mtfs_ra_rtc_provider(mtfs_ra_rtc_context_t *context)
{
    if ((context == NULL) || !context->initialized) {
        return NULL;
    }
    return &context->provider;
}

#else

typedef int mtfs_ra_rtc_provider_not_selected_t;

#endif
