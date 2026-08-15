/* UART-neutral command parser for the embedded RTC setting application. */
#ifndef MTFS_RTC_SET_APP_H
#define MTFS_RTC_SET_APP_H

#include <stddef.h>

typedef void (*mtfs_rtc_set_write_t)(void *context, const char *text);
typedef int (*mtfs_rtc_set_command_t)(void *context, const char *line);

typedef struct mtfs_rtc_set_app
{
    mtfs_rtc_set_write_t write;
    void *write_context;
    mtfs_rtc_set_command_t command;
    void *command_context;
    const char *command_help;
    char line[64];
    size_t length;
    int ignore_next_lf;
} mtfs_rtc_set_app_t;

void mtfs_rtc_set_app_init(
    mtfs_rtc_set_app_t *app,
    mtfs_rtc_set_write_t write,
    void *write_context);
void mtfs_rtc_set_app_set_extension(
    mtfs_rtc_set_app_t *app,
    mtfs_rtc_set_command_t command,
    void *command_context,
    const char *command_help);
void mtfs_rtc_set_app_banner(mtfs_rtc_set_app_t *app);
void mtfs_rtc_set_app_feed(mtfs_rtc_set_app_t *app, char character);
void mtfs_rtc_set_app_execute(mtfs_rtc_set_app_t *app, const char *line);

#endif /* MTFS_RTC_SET_APP_H */
