/* UART-neutral RTC commands and extension dispatch for diagnostic consoles. */
#ifndef MTFS_CONSOLE_H
#define MTFS_CONSOLE_H

#include <stddef.h>

typedef void (*mtfs_console_write_t)(void *context, const char *text);
typedef int (*mtfs_console_command_t)(void *context, const char *line);

typedef struct mtfs_console
{
    mtfs_console_write_t write;
    void *write_context;
    mtfs_console_command_t command;
    void *command_context;
    const char *command_help;
    char line[64];
    size_t length;
    int ignore_next_lf;
} mtfs_console_t;

void mtfs_console_init(
    mtfs_console_t *console,
    mtfs_console_write_t write,
    void *write_context);
void mtfs_console_set_extension(
    mtfs_console_t *console,
    mtfs_console_command_t command,
    void *command_context,
    const char *command_help);
void mtfs_console_banner(mtfs_console_t *console);
void mtfs_console_feed(mtfs_console_t *console, char character);
void mtfs_console_execute(mtfs_console_t *console, const char *line);

#endif /* MTFS_CONSOLE_H */
