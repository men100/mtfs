#ifndef TEST_FATFS_CONCURRENT_H
#define TEST_FATFS_CONCURRENT_H

#include "../framework/mtfs_test.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runs POSIX worker threads against an already formatted, unmounted volume. */
int test_fatfs_concurrent(mtfs_test_t *test, const char *volume_path);

#ifdef __cplusplus
}
#endif

#endif /* TEST_FATFS_CONCURRENT_H */
