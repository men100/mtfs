#ifndef MTFS_SENTINEL_LAB_CONSOLE_H
#define MTFS_SENTINEL_LAB_CONSOLE_H

#include <stddef.h>

typedef void (*mtfs_sentinel_lab_console_write_fn)(
    void *context, const char *text);
typedef int (*mtfs_sentinel_lab_console_command_fn)(
    void *context, const char *line);

typedef struct mtfs_sentinel_lab_console
{
    mtfs_sentinel_lab_console_write_fn write;
    void *write_context;
    mtfs_sentinel_lab_console_command_fn command;
    void *command_context;
    char line[80];
    size_t length;
    int ignore_next_lf;
} mtfs_sentinel_lab_console_t;

void mtfs_sentinel_lab_console_init(
    mtfs_sentinel_lab_console_t *console,
    mtfs_sentinel_lab_console_write_fn write,
    void *write_context,
    mtfs_sentinel_lab_console_command_fn command,
    void *command_context);
void mtfs_sentinel_lab_console_feed(
    mtfs_sentinel_lab_console_t *console, char character);

#endif
