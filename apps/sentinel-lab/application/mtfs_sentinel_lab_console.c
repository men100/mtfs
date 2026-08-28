#include "mtfs_sentinel_lab_console.h"

#include <string.h>

static void console_write(
    mtfs_sentinel_lab_console_t *console, const char *text)
{
    if (console->write != NULL)
        console->write(console->write_context, text);
}

void mtfs_sentinel_lab_console_init(
    mtfs_sentinel_lab_console_t *console,
    mtfs_sentinel_lab_console_write_fn write,
    void *write_context,
    mtfs_sentinel_lab_console_command_fn command,
    void *command_context)
{
    if (console == NULL) return;
    (void)memset(console, 0, sizeof(*console));
    console->write = write;
    console->write_context = write_context;
    console->command = command;
    console->command_context = command_context;
}

void mtfs_sentinel_lab_console_feed(
    mtfs_sentinel_lab_console_t *console, char character)
{
    if (console == NULL) return;
    if (character == '\n' && console->ignore_next_lf) {
        console->ignore_next_lf = 0;
        return;
    }
    console->ignore_next_lf = character == '\r';
    if (character == '\r' || character == '\n') {
        console_write(console, "\r\n");
        if (console->length != 0U) {
            console->line[console->length] = '\0';
            if (console->command == NULL ||
                !console->command(console->command_context, console->line))
                console_write(console,
                    "ERROR: unknown command; type help\r\n");
            console->length = 0U;
        }
        console_write(console, "> ");
    } else if (character == '\b' || character == 0x7f) {
        if (console->length != 0U) {
            --console->length;
            console_write(console, "\b \b");
        }
    } else if (character >= ' ' && character <= '~') {
        if (console->length + 1U < sizeof(console->line)) {
            char echo[2] = {character, '\0'};
            console->line[console->length++] = character;
            console_write(console, echo);
        } else {
            console->length = 0U;
            console_write(console,
                "\r\nERROR: input line too long\r\n> ");
        }
    }
}
