#include <stddef.h>
#include <string.h>

#include "mtfs_sentinel_lab_console.h"

static char output[512];
static size_t output_length;
static unsigned int command_calls;
static unsigned int record_calls;

static void capture_write(void *context, const char *text)
{
    size_t length = strlen(text);
    (void)context;
    if (output_length + length < sizeof(output)) {
        memcpy(output + output_length, text, length + 1U);
        output_length += length;
    }
}

static int dispatch(void *context, const char *line)
{
    (void)context;
    ++command_calls;
    if (strcmp(line, "record") != 0) return 0;
    ++record_calls;
    return 1;
}

static void feed(mtfs_sentinel_lab_console_t *console, const char *text)
{
    while (*text != '\0')
        mtfs_sentinel_lab_console_feed(console, *text++);
}

int main(void)
{
    mtfs_sentinel_lab_console_t console;
    mtfs_sentinel_lab_console_init(&console, capture_write, NULL,
        dispatch, NULL);
    feed(&console, "record\r\n");
    if (command_calls != 1U || record_calls != 1U) return 1;
    feed(&console, "recx\bord\r");
    if (command_calls != 2U || record_calls != 2U) return 1;
    feed(&console, "unknown\n");
    if (command_calls != 3U || record_calls != 2U ||
        strstr(output, "ERROR: unknown command; type help") == NULL)
        return 1;
    mtfs_sentinel_lab_console_init(NULL, NULL, NULL, NULL, NULL);
    mtfs_sentinel_lab_console_feed(NULL, 'x');
    return 0;
}
