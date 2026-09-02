#include <stdio.h>

#include "mtfs_test.h"
#include "test_sentinel_inference.h"

static void reporter(void *context, mtfs_test_event_t event,
    const char *test_name, const char *file, int line, const char *message,
    unsigned int checks, unsigned int failures)
{
    (void)context; (void)test_name; (void)checks; (void)failures;
    if (event == MTFS_TEST_EVENT_CHECK_FAILED)
        printf("%s:%d: %s\n", file, line, message);
}

int main(void)
{
    mtfs_test_t test;
    mtfs_test_begin(&test, "sentinel bundle and CPU inference", reporter, NULL);
    if (test_sentinel_inference(&test) != 0) return 1;
    return mtfs_test_finish(&test);
}
