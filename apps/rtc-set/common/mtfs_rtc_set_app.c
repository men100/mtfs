#include "mtfs_rtc_set_app.h"

#include <string.h>

#include "mtfs_time.h"

static void app_write(mtfs_rtc_set_app_t *app, const char *text)
{
    if ((app != NULL) && (app->write != NULL)) {
        app->write(app->write_context, text);
    }
}

static const char *app_status_name(mtfs_time_status_t status)
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

static void app_append_two(char *output, size_t *offset, unsigned int value)
{
    output[(*offset)++] = (char)('0' + ((value / 10U) % 10U));
    output[(*offset)++] = (char)('0' + (value % 10U));
}

static void app_append_four(char *output, size_t *offset, unsigned int value)
{
    output[(*offset)++] = (char)('0' + ((value / 1000U) % 10U));
    output[(*offset)++] = (char)('0' + ((value / 100U) % 10U));
    output[(*offset)++] = (char)('0' + ((value / 10U) % 10U));
    output[(*offset)++] = (char)('0' + (value % 10U));
}

static void app_print_datetime(
    mtfs_rtc_set_app_t *app, const char *prefix,
    const mtfs_datetime_t *datetime, mtfs_time_status_t status)
{
    char output[80];
    size_t offset = 0U;
    const char *name = app_status_name(status);

    while (*prefix != '\0') {
        output[offset++] = *prefix++;
    }
    while (*name != '\0') {
        output[offset++] = *name++;
    }
    if (status == MTFS_TIME_STATUS_VALID) {
        output[offset++] = ' ';
        app_append_four(output, &offset, datetime->year);
        output[offset++] = '-';
        app_append_two(output, &offset, datetime->month);
        output[offset++] = '-';
        app_append_two(output, &offset, datetime->day);
        output[offset++] = ' ';
        app_append_two(output, &offset, datetime->hour);
        output[offset++] = ':';
        app_append_two(output, &offset, datetime->minute);
        output[offset++] = ':';
        app_append_two(output, &offset, datetime->second);
    }
    output[offset++] = '\r';
    output[offset++] = '\n';
    output[offset] = '\0';
    app_write(app, output);
}

static void app_print_status(
    mtfs_rtc_set_app_t *app, mtfs_time_status_t status)
{
    char output[32] = "status: ";
    size_t offset = 8U;
    const char *name = app_status_name(status);
    while (*name != '\0') {
        output[offset++] = *name++;
    }
    output[offset++] = '\r';
    output[offset++] = '\n';
    output[offset] = '\0';
    app_write(app, output);
}

static int app_parse_digits(const char *text, size_t count, unsigned int *value)
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

static int app_parse_datetime(const char *text, mtfs_datetime_t *datetime)
{
    unsigned int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second;

    if ((strlen(text) != 19U) || (text[4] != '-') || (text[7] != '-') ||
        (text[10] != ' ') || (text[13] != ':') || (text[16] != ':') ||
        !app_parse_digits(text, 4U, &year) ||
        !app_parse_digits(text + 5, 2U, &month) ||
        !app_parse_digits(text + 8, 2U, &day) ||
        !app_parse_digits(text + 11, 2U, &hour) ||
        !app_parse_digits(text + 14, 2U, &minute) ||
        !app_parse_digits(text + 17, 2U, &second)) {
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

static void app_get(mtfs_rtc_set_app_t *app, const char *prefix)
{
    mtfs_datetime_t datetime = {0U, 0U, 0U, 0U, 0U, 0U};
    mtfs_time_status_t status;
    mtfs_error_t error = mtfs_time_get_local(&datetime, &status);
    if ((error != MTFS_OK) && (status != MTFS_TIME_STATUS_UNAVAILABLE)) {
        status = MTFS_TIME_STATUS_ERROR;
    }
    app_print_datetime(app, prefix, &datetime, status);
}

void mtfs_rtc_set_app_init(
    mtfs_rtc_set_app_t *app,
    mtfs_rtc_set_write_t write,
    void *write_context)
{
    if (app == NULL) {
        return;
    }
    memset(app, 0, sizeof(*app));
    app->write = write;
    app->write_context = write_context;
}

void mtfs_rtc_set_app_set_extension(
    mtfs_rtc_set_app_t *app,
    mtfs_rtc_set_command_t command,
    void *command_context,
    const char *command_help)
{
    if (app == NULL) {
        return;
    }
    app->command = command;
    app->command_context = command_context;
    app->command_help = command_help;
}

void mtfs_rtc_set_app_banner(mtfs_rtc_set_app_t *app)
{
    app_write(app,
        "microT-FS RTC set (local time; no timezone/DST conversion)\r\n"
        "Type help for commands.\r\n> ");
}

void mtfs_rtc_set_app_execute(mtfs_rtc_set_app_t *app, const char *line)
{
    mtfs_datetime_t datetime;
    mtfs_time_status_t status;
    mtfs_error_t error;

    if ((app == NULL) || (line == NULL)) {
        return;
    }
    if (strcmp(line, "help") == 0) {
        app_write(app,
            "help                          show this help\r\n"
            "rtc-get                       show local RTC time\r\n"
            "rtc-status                    show provider state\r\n"
            "rtc-set YYYY-MM-DD hh:mm:ss    set local time and verify it\r\n"
            "rtc-clear                     clear the setting marker\r\n");
        if (app->command_help != NULL) {
            app_write(app, app->command_help);
        }
    } else if (strcmp(line, "rtc-get") == 0) {
        app_get(app, "time: ");
    } else if (strcmp(line, "rtc-status") == 0) {
        error = mtfs_time_get_status(&status);
        if (error != MTFS_OK) {
            status = MTFS_TIME_STATUS_ERROR;
        }
        app_print_status(app, status);
    } else if (strcmp(line, "rtc-clear") == 0) {
        error = mtfs_time_clear();
        app_write(app, error == MTFS_OK ?
            "OK: marker cleared; state is UNSET\r\n" :
            "ERROR: marker clear failed\r\n");
    } else if (strncmp(line, "rtc-set ", 8U) == 0) {
        if (!app_parse_datetime(line + 8, &datetime)) {
            app_write(app,
                "ERROR: use rtc-set YYYY-MM-DD hh:mm:ss with a valid date\r\n");
        } else {
            error = mtfs_time_set_local(&datetime);
            if (error == MTFS_OK) {
                app_write(app, "OK: RTC set and marker committed\r\n");
                app_get(app, "readback: ");
            } else if (error == MTFS_ERROR_OUT_OF_RANGE) {
                app_write(app, "ERROR: date is outside the target RTC range\r\n");
            } else {
                app_write(app,
                    "ERROR: RTC set/readback failed; marker remains clear\r\n");
            }
        }
    } else if ((line[0] != '\0') &&
        ((app->command == NULL) ||
            !app->command(app->command_context, line))) {
        app_write(app, "ERROR: unknown command; type help\r\n");
    }
}

void mtfs_rtc_set_app_feed(mtfs_rtc_set_app_t *app, char character)
{
    if (app == NULL) {
        return;
    }

    if ((character == '\n') && app->ignore_next_lf) {
        app->ignore_next_lf = 0;
        return;
    }
    app->ignore_next_lf = character == '\r';

    if ((character == '\r') || (character == '\n')) {
        app_write(app, "\r\n");
        if (app->length != 0U) {
            app->line[app->length] = '\0';
            mtfs_rtc_set_app_execute(app, app->line);
            app->length = 0U;
        }
        app_write(app, "> ");
    } else if ((character == '\b') || (character == 0x7FU)) {
        if (app->length != 0U) {
            --app->length;
            app_write(app, "\b \b");
        }
    } else if ((character >= ' ') && (character <= '~')) {
        if (app->length + 1U < sizeof(app->line)) {
            char echo[2] = {character, '\0'};
            app->line[app->length++] = character;
            app_write(app, echo);
        } else {
            app->length = 0U;
            app_write(app, "\r\nERROR: input line too long\r\n> ");
        }
    }
}
