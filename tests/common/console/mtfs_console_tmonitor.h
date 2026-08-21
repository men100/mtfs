/* T-Monitor transport adapter for the shared diagnostic console parser. */
#ifndef MTFS_CONSOLE_TMONITOR_H
#define MTFS_CONSOLE_TMONITOR_H

#ifdef __cplusplus
extern "C" {
#endif

void mtfs_console_tmonitor_write(void *context, const char *text);
int mtfs_console_tmonitor_getchar(void);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_CONSOLE_TMONITOR_H */
