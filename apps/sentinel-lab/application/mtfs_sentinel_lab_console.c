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
    console->log_level = MTFS_APP_LOG_INFO;
}

void mtfs_sentinel_lab_console_execute(
    mtfs_sentinel_lab_console_t *console, const char *line)
{
    mtfs_app_log_level_t requested_level;
    int is_query;
    int result;
    char output[] = "log-level set: debug\r\n";
    const char *prefix;
    const char *name;
    size_t offset = 0U;

    if ((console == NULL) || (line == NULL)) return;
    requested_level = console->log_level;
    result = mtfs_app_log_parse_command(line, &requested_level, &is_query);
    if (result < 0) {
        console_write(console,
            "ERROR: use log-level off|error|info|debug\r\n");
        return;
    }
    if (result > 0) {
        if (!is_query) console->log_level = requested_level;
        prefix = is_query ? "log-level: " : "log-level set: ";
        name = mtfs_app_log_level_name(console->log_level);
        while (*prefix != '\0') output[offset++] = *prefix++;
        while (*name != '\0') output[offset++] = *name++;
        output[offset++] = '\r';
        output[offset++] = '\n';
        output[offset] = '\0';
        console_write(console, output);
        return;
    }
    if ((line[0] != '\0') &&
        (console->command == NULL ||
            !console->command(console->command_context, line))) {
        console_write(console, "ERROR: unknown command; type help\r\n");
    }
}

mtfs_app_log_level_t mtfs_sentinel_lab_console_log_level(
    const mtfs_sentinel_lab_console_t *console)
{
    return console == NULL ? MTFS_APP_LOG_INFO : console->log_level;
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
            mtfs_sentinel_lab_console_execute(console, console->line);
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
