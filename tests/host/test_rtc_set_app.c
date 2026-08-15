#include "test_rtc_set_app.h"

#include <stddef.h>
#include <string.h>

#include "mtfs_rtc_set_app.h"

typedef struct rtc_set_fixture
{
    char output[256];
    size_t output_length;
    char command[64];
    unsigned int command_count;
} rtc_set_fixture_t;

static void fixture_write(void *opaque, const char *text)
{
    rtc_set_fixture_t *fixture = (rtc_set_fixture_t *)opaque;
    size_t length = strlen(text);
    size_t remaining = sizeof(fixture->output) - fixture->output_length - 1U;

    if (length > remaining) {
        length = remaining;
    }
    memcpy(&fixture->output[fixture->output_length], text, length);
    fixture->output_length += length;
    fixture->output[fixture->output_length] = '\0';
}

static int fixture_command(void *opaque, const char *line)
{
    rtc_set_fixture_t *fixture = (rtc_set_fixture_t *)opaque;

    ++fixture->command_count;
    strncpy(fixture->command, line, sizeof(fixture->command) - 1U);
    fixture->command[sizeof(fixture->command) - 1U] = '\0';
    return 1;
}

static void fixture_feed(mtfs_rtc_set_app_t *app, const char *text)
{
    while (*text != '\0') {
        mtfs_rtc_set_app_feed(app, *text++);
    }
}

int test_rtc_set_app(mtfs_test_t *test)
{
    mtfs_rtc_set_app_t app;
    rtc_set_fixture_t fixture;

    memset(&fixture, 0, sizeof(fixture));
    mtfs_rtc_set_app_init(&app, fixture_write, &fixture);
    mtfs_rtc_set_app_set_extension(
        &app, fixture_command, &fixture, NULL);
    fixture_feed(&app, "probe\r\n");
    if (!MTFS_TEST_CHECK(test,
            fixture.command_count == 1U &&
                strcmp(fixture.command, "probe") == 0 &&
                strcmp(fixture.output, "probe\r\n> ") == 0,
            "RTC console treats CR+LF as one command terminator")) {
        return 1;
    }

    memset(&fixture, 0, sizeof(fixture));
    mtfs_rtc_set_app_init(&app, fixture_write, &fixture);
    mtfs_rtc_set_app_set_extension(
        &app, fixture_command, &fixture, NULL);
    fixture_feed(&app, "\r\n");
    if (!MTFS_TEST_CHECK(test,
            fixture.command_count == 0U &&
                strcmp(fixture.output, "\r\n> ") == 0,
            "RTC console starts a new line for an empty command")) {
        return 1;
    }

    memset(&fixture, 0, sizeof(fixture));
    mtfs_rtc_set_app_init(&app, fixture_write, &fixture);
    mtfs_rtc_set_app_set_extension(
        &app, fixture_command, &fixture, NULL);
    fixture_feed(&app, "abc\b\r");
    if (!MTFS_TEST_CHECK(test,
            fixture.command_count == 1U &&
                strcmp(fixture.command, "ab") == 0 &&
                strcmp(fixture.output, "abc\b \b\r\n> ") == 0,
            "RTC console edits input with backspace")) {
        return 1;
    }

    return 0;
}
