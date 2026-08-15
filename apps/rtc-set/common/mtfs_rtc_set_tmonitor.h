/* T-Monitor transport adapter for the RTC setting application. */
#ifndef MTFS_RTC_SET_TMONITOR_H
#define MTFS_RTC_SET_TMONITOR_H

#ifdef __cplusplus
extern "C" {
#endif

void mtfs_rtc_set_tmonitor_write(void *context, const char *text);
int mtfs_rtc_set_tmonitor_getchar(void);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_RTC_SET_TMONITOR_H */
