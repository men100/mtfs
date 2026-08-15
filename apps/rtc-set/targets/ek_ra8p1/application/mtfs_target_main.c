#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "hal_data.h"
#include "mtfs_ra_rtc.h"
#include "mtfs_rtc_set_app.h"
#include "mtfs_rtc_set_tmonitor.h"

static mtfs_ra_rtc_context_t rtc_context;

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
    mtfs_error_t error;
    mtfs_time_status_t status = MTFS_TIME_STATUS_UNAVAILABLE;

    error = mtfs_ra_rtc_init(&rtc_context, &g_rtc0, NULL, NULL, NULL);
    if (error == MTFS_OK) {
        error = mtfs_time_provider_register(
            mtfs_ra_rtc_provider(&rtc_context));
    }
    if (error != MTFS_OK) {
        tm_printf((UB *)"[mtfs] RTC provider init FAIL: %d fsp=%d vbt=0x%02x\n",
            error, rtc_context.last_fsp_error,
            rtc_context.vbatt_status_at_init);
        for (;;) {
            (void)tk_slp_tsk(TMO_FEVR);
        }
    }

    (void)mtfs_time_get_status(&status);
    tm_printf((UB *)"[mtfs] EK-RA8P1 standalone RTC console\n");
    tm_printf((UB *)"[mtfs] RTC provider state=%u source=SUBCLK local-time vbt=0x%02x cold=%u source-init=%u\n",
        (UW)status, rtc_context.vbatt_status_at_init,
        (UW)rtc_context.backup_power_loss_at_init,
        (UW)rtc_context.clock_source_initialized);
    rtc_console_run();
    return 0;
}
