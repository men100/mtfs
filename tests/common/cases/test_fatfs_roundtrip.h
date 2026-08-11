#ifndef TEST_FATFS_ROUNDTRIP_H
#define TEST_FATFS_ROUNDTRIP_H

#include "../framework/mtfs_test.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runs against an already formatted volume. This function never formats media. */
int test_fatfs_roundtrip(mtfs_test_t *test, const char *volume_path);

#ifdef __cplusplus
}
#endif

#endif /* TEST_FATFS_ROUNDTRIP_H */
