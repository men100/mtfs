/* Renesas RA FSP calendar RTC provider for microT-FS. */
#ifndef MTFS_RA_RTC_H
#define MTFS_RA_RTC_H

#include "r_rtc_api.h"

#include "../../../core/mtfs_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The final 12 bytes of the 128-byte RA8P1 VBATT backup register area. */
#define MTFS_RA_RTC_MARKER_OFFSET_VERSION  (116U)
#define MTFS_RA_RTC_MARKER_OFFSET_INVERSE  (120U)
#define MTFS_RA_RTC_MARKER_OFFSET_MAGIC    (124U)

typedef struct mtfs_ra_rtc_context
{
    rtc_instance_t const *rtc;
    mtfs_time_provider_t provider;
    fsp_err_t last_fsp_error;
    uint8_t vbatt_status_at_init;
    int backup_power_loss_at_init;
    int clock_source_initialized;
    int initialized;
} mtfs_ra_rtc_context_t;

mtfs_error_t mtfs_ra_rtc_init(
    mtfs_ra_rtc_context_t *context,
    rtc_instance_t const *rtc,
    mtfs_error_t (*lock)(void *lock_context),
    void (*unlock)(void *lock_context),
    void *lock_context);

mtfs_time_provider_t *mtfs_ra_rtc_provider(mtfs_ra_rtc_context_t *context);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_RA_RTC_H */
