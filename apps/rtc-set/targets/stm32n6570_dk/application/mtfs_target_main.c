#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "mtfs_rtc_set_app.h"
#include "mtfs_rtc_set_tmonitor.h"
#include "mtfs_stm32_rtc.h"

static mtfs_stm32_rtc_context_t rtc_context;
static ID rtc_mutex_id;

static mtfs_error_t rtc_lock(void *opaque)
{
    ID mutex_id = *(ID *)opaque;
    return tk_loc_mtx(mutex_id, TMO_FEVR) == E_OK ?
        MTFS_OK : MTFS_ERROR_IO;
}

static void rtc_unlock(void *opaque)
{
    ID mutex_id = *(ID *)opaque;
    (void)tk_unl_mtx(mutex_id);
}

static void rtc_console_run(void)
{
    mtfs_rtc_set_app_t app;

    mtfs_rtc_set_app_init(&app, mtfs_rtc_set_tmonitor_write, NULL);
    mtfs_rtc_set_app_banner(&app);
    for (;;) {
        mtfs_rtc_set_app_feed(&app,
            (char)mtfs_rtc_set_tmonitor_getchar());
    }
}

EXPORT INT usermain(void);

EXPORT INT usermain(void)
{
    T_CMTX mutex_config = {
        .mtxatr = TA_INHERIT
    };
    mtfs_error_t error;
    mtfs_time_status_t status = MTFS_TIME_STATUS_UNAVAILABLE;

    rtc_mutex_id = tk_cre_mtx(&mutex_config);
    if (rtc_mutex_id <= 0) {
        tm_printf((UB *)"[mtfs] RTC mutex create FAIL: %d\n",
            rtc_mutex_id);
        goto park;
    }

    error = mtfs_stm32_rtc_init(
        &rtc_context, rtc_lock, rtc_unlock, &rtc_mutex_id);
    if (error == MTFS_OK) {
        error = mtfs_time_provider_register(
            mtfs_stm32_rtc_provider(&rtc_context));
    }
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] RTC provider init FAIL: %d reset=0x%08x\n",
            error, rtc_context.reset_flags_at_init);
        goto park;
    }

    (void)mtfs_time_get_status(&status);
    tm_printf((UB *)"[mtfs] STM32N6570-DK standalone RTC console\n");
    tm_printf((UB *)"[mtfs] RTC provider state=%u source=LSI local-time reset=0x%08x\n",
        (UW)status, rtc_context.reset_flags_at_init);
    rtc_console_run();

park:
    for (;;) {
        (void)tk_slp_tsk(TMO_FEVR);
    }
}
