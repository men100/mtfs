#ifndef TEST_FATFS_TIMESTAMP_H
#define TEST_FATFS_TIMESTAMP_H

#include <stdint.h>

#include "mtfs_test.h"
#include "mtfs_time.h"

typedef struct mtfs_fatfs_timestamp_result
{
    mtfs_datetime_t rtc_before;
    mtfs_datetime_t file_time;
    mtfs_datetime_t rtc_after;
    uint32_t fat_before;
    uint32_t fat_file;
    uint32_t fat_after;
} mtfs_fatfs_timestamp_result_t;

int test_fatfs_timestamp(
    mtfs_test_t *test,
    const char *volume_path,
    mtfs_fatfs_timestamp_result_t *timestamp_result);

#endif /* TEST_FATFS_TIMESTAMP_H */
