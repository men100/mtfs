#include "mtfs_test.h"

static void mtfs_test_report(
    const mtfs_test_t *test,
    mtfs_test_event_t event,
    const char *file,
    int line,
    const char *message)
{
    if (test->reporter != 0) {
        test->reporter(test->reporter_context, event, test->name, file, line,
            message, test->checks, test->failures);
    }
}

void mtfs_test_begin(
    mtfs_test_t *test,
    const char *name,
    mtfs_test_reporter_t reporter,
    void *reporter_context)
{
    test->name = name;
    test->checks = 0U;
    test->failures = 0U;
    test->reporter = reporter;
    test->reporter_context = reporter_context;
    mtfs_test_report(test, MTFS_TEST_EVENT_BEGIN, 0, 0, 0);
}

int mtfs_test_check(
    mtfs_test_t *test,
    int condition,
    const char *file,
    int line,
    const char *message)
{
    ++test->checks;
    if (condition) {
        return 1;
    }
    ++test->failures;
    mtfs_test_report(test, MTFS_TEST_EVENT_CHECK_FAILED, file, line, message);
    return 0;
}

int mtfs_test_finish(const mtfs_test_t *test)
{
    mtfs_test_report(test, MTFS_TEST_EVENT_FINISH, 0, 0, 0);
    return (test->failures == 0U) ? 0 : 1;
}
