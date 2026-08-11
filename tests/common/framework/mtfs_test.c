#include "mtfs_test.h"

#include <stdio.h>

void mtfs_test_begin(mtfs_test_t *test, const char *name)
{
    test->name = name;
    test->checks = 0U;
    test->failures = 0U;
    printf("[ RUN  ] %s\n", name);
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
    printf("[ FAIL ] %s:%d: %s\n", file, line, message);
    return 0;
}

int mtfs_test_finish(const mtfs_test_t *test)
{
    if (test->failures == 0U) {
        printf("[ PASS ] %s (%u checks)\n", test->name, test->checks);
        return 0;
    }
    printf("[ FAIL ] %s (%u failures, %u checks)\n",
        test->name, test->failures, test->checks);
    return 1;
}
