/* Shared RTC commands and extension dispatch for diagnostic consoles. */
#include "mtfs_console.h"

#include <string.h>

#include "mtfs_time.h"

static void console_write(mtfs_console_t *console, const char *text)
{
    if ((console != NULL) && (console->write != NULL)) {
        console->write(console->write_context, text);
    }
}

static const char *console_status_name(mtfs_time_status_t status)
{
    switch (status) {
    case MTFS_TIME_STATUS_VALID:
        return "VALID";
    case MTFS_TIME_STATUS_UNSET:
        return "UNSET";
    case MTFS_TIME_STATUS_ERROR:
        return "ERROR";
    case MTFS_TIME_STATUS_UNAVAILABLE:
        return "UNAVAILABLE";
    default:
        return "ERROR";
    }
}

static void console_append_two(char *output, size_t *offset, unsigned int value)
{
    output[(*offset)++] = (char)('0' + ((value / 10U) % 10U));
    output[(*offset)++] = (char)('0' + (value % 10U));
}

static void console_append_four(char *output, size_t *offset, unsigned int value)
{
    output[(*offset)++] = (char)('0' + ((value / 1000U) % 10U));
    output[(*offset)++] = (char)('0' + ((value / 100U) % 10U));
    output[(*offset)++] = (char)('0' + ((value / 10U) % 10U));
    output[(*offset)++] = (char)('0' + (value % 10U));
}

static void console_print_datetime(
    mtfs_console_t *console, const char *prefix,
    const mtfs_datetime_t *datetime, mtfs_time_status_t status)
{
    char output[80];
    size_t offset = 0U;
    const char *name = console_status_name(status);

    while (*prefix != '\0') {
        output[offset++] = *prefix++;
    }
    while (*name != '\0') {
        output[offset++] = *name++;
    }
    if (status == MTFS_TIME_STATUS_VALID) {
        output[offset++] = ' ';
        console_append_four(output, &offset, datetime->year);
        output[offset++] = '-';
        console_append_two(output, &offset, datetime->month);
        output[offset++] = '-';
        console_append_two(output, &offset, datetime->day);
        output[offset++] = ' ';
        console_append_two(output, &offset, datetime->hour);
        output[offset++] = ':';
        console_append_two(output, &offset, datetime->minute);
        output[offset++] = ':';
        console_append_two(output, &offset, datetime->second);
    }
    output[offset++] = '\r';
    output[offset++] = '\n';
    output[offset] = '\0';
    console_write(console, output);
}

static void console_print_status(
    mtfs_console_t *console, mtfs_time_status_t status)
{
    char output[32] = "status: ";
    size_t offset = 8U;
    const char *name = console_status_name(status);
    while (*name != '\0') {
        output[offset++] = *name++;
    }
    output[offset++] = '\r';
    output[offset++] = '\n';
    output[offset] = '\0';
    console_write(console, output);
}

static int console_parse_digits(const char *text, size_t count, unsigned int *value)
{
    size_t index;
    unsigned int result = 0U;
    for (index = 0U; index < count; ++index) {
        if ((text[index] < '0') || (text[index] > '9')) {
            return 0;
        }
        result = result * 10U + (unsigned int)(text[index] - '0');
    }
    *value = result;
    return 1;
}

static int console_parse_datetime(const char *text, mtfs_datetime_t *datetime)
{
    unsigned int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second;

    if ((strlen(text) != 19U) || (text[4] != '-') || (text[7] != '-') ||
        (text[10] != ' ') || (text[13] != ':') || (text[16] != ':') ||
        !console_parse_digits(text, 4U, &year) ||
        !console_parse_digits(text + 5, 2U, &month) ||
        !console_parse_digits(text + 8, 2U, &day) ||
        !console_parse_digits(text + 11, 2U, &hour) ||
        !console_parse_digits(text + 14, 2U, &minute) ||
        !console_parse_digits(text + 17, 2U, &second)) {
        return 0;
    }
    datetime->year = (uint16_t)year;
    datetime->month = (uint8_t)month;
    datetime->day = (uint8_t)day;
    datetime->hour = (uint8_t)hour;
    datetime->minute = (uint8_t)minute;
    datetime->second = (uint8_t)second;
    return mtfs_datetime_is_valid(datetime);
}

static void console_get(mtfs_console_t *console, const char *prefix)
{
    mtfs_datetime_t datetime = {0U, 0U, 0U, 0U, 0U, 0U};
    mtfs_time_status_t status;
    mtfs_error_t error = mtfs_time_get_local(&datetime, &status);
    if ((error != MTFS_OK) && (status != MTFS_TIME_STATUS_UNAVAILABLE)) {
        status = MTFS_TIME_STATUS_ERROR;
    }
    console_print_datetime(console, prefix, &datetime, status);
}

void mtfs_console_init(
    mtfs_console_t *console,
    mtfs_console_write_t write,
    void *write_context)
{
    if (console == NULL) {
        return;
    }
    memset(console, 0, sizeof(*console));
    console->write = write;
    console->write_context = write_context;
}

void mtfs_console_set_extension(
    mtfs_console_t *console,
    mtfs_console_command_t command,
    void *command_context,
    const char *command_help)
{
    if (console == NULL) {
        return;
    }
    console->command = command;
    console->command_context = command_context;
    console->command_help = command_help;
}

void mtfs_console_banner(mtfs_console_t *console)
{
    console_write(console,
        "microT-FS RTC set (local time; no timezone/DST conversion)\r\n"
        "Type help for commands.\r\n> ");
}

void mtfs_console_execute(mtfs_console_t *console, const char *line)
{
    mtfs_datetime_t datetime;
    mtfs_time_status_t status;
    mtfs_error_t error;

    if ((console == NULL) || (line == NULL)) {
        return;
    }
    if (strcmp(line, "help") == 0) {
        console_write(console,
            "help                          show this help\r\n"
            "rtc-get                       show local RTC time\r\n"
            "rtc-status                    show provider state\r\n"
            "rtc-set YYYY-MM-DD hh:mm:ss    set local time and verify it\r\n"
            "rtc-clear                     clear the setting marker\r\n");
        if (console->command_help != NULL) {
            console_write(console, console->command_help);
        }
    } else if (strcmp(line, "rtc-get") == 0) {
        console_get(console, "time: ");
    } else if (strcmp(line, "rtc-status") == 0) {
        error = mtfs_time_get_status(&status);
        if (error != MTFS_OK) {
            status = MTFS_TIME_STATUS_ERROR;
        }
        console_print_status(console, status);
    } else if (strcmp(line, "rtc-clear") == 0) {
        error = mtfs_time_clear();
        console_write(console, error == MTFS_OK ?
            "OK: marker cleared; state is UNSET\r\n" :
            "ERROR: marker clear failed\r\n");
    } else if (strncmp(line, "rtc-set ", 8U) == 0) {
        if (!console_parse_datetime(line + 8, &datetime)) {
            console_write(console,
                "ERROR: use rtc-set YYYY-MM-DD hh:mm:ss with a valid date\r\n");
        } else {
            error = mtfs_time_set_local(&datetime);
            if (error == MTFS_OK) {
                console_write(console, "OK: RTC set and marker committed\r\n");
                console_get(console, "readback: ");
            } else if (error == MTFS_ERROR_OUT_OF_RANGE) {
                console_write(console, "ERROR: date is outside the target RTC range\r\n");
            } else {
                console_write(console,
                    "ERROR: RTC set/readback failed; marker remains clear\r\n");
            }
        }
    } else if ((line[0] != '\0') &&
        ((console->command == NULL) ||
            !console->command(console->command_context, line))) {
        console_write(console, "ERROR: unknown command; type help\r\n");
    }
}

void mtfs_console_feed(mtfs_console_t *console, char character)
{
    if (console == NULL) {
        return;
    }

    if ((character == '\n') && console->ignore_next_lf) {
        console->ignore_next_lf = 0;
        return;
    }
    console->ignore_next_lf = character == '\r';

    if ((character == '\r') || (character == '\n')) {
        console_write(console, "\r\n");
        if (console->length != 0U) {
            console->line[console->length] = '\0';
            mtfs_console_execute(console, console->line);
            console->length = 0U;
        }
        console_write(console, "> ");
    } else if ((character == '\b') || (character == 0x7FU)) {
        if (console->length != 0U) {
            --console->length;
            console_write(console, "\b \b");
        }
    } else if ((character >= ' ') && (character <= '~')) {
        if (console->length + 1U < sizeof(console->line)) {
            char echo[2] = {character, '\0'};
            console->line[console->length++] = character;
            console_write(console, echo);
        } else {
            console->length = 0U;
            console_write(console, "\r\nERROR: input line too long\r\n> ");
        }
    }
}
