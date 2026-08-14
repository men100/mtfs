#include "test_fatfs_timestamp.h"

#include <stddef.h>
#include <string.h>

#include "ff.h"

#define MTFS_TIMESTAMP_PATH_SIZE (64U)

static int mtfs_timestamp_make_path(
    char *destination,
    size_t destination_size,
    const char *volume_path)
{
    static const char filename[] = "MTFSTIME.TST";
    size_t volume_length;

    if ((destination == NULL) || (volume_path == NULL)) {
        return 0;
    }
    volume_length = strlen(volume_path);
    if ((destination_size < sizeof(filename)) ||
        (volume_length > (destination_size - sizeof(filename)))) {
        return 0;
    }
    memcpy(destination, volume_path, volume_length);
    memcpy(destination + volume_length, filename, sizeof(filename));
    return 1;
}

static void mtfs_timestamp_decode(uint32_t timestamp, mtfs_datetime_t *datetime)
{
    datetime->year = (uint16_t)(1980U + ((timestamp >> 25) & 0x7FU));
    datetime->month = (uint8_t)((timestamp >> 21) & 0x0FU);
    datetime->day = (uint8_t)((timestamp >> 16) & 0x1FU);
    datetime->hour = (uint8_t)((timestamp >> 11) & 0x1FU);
    datetime->minute = (uint8_t)((timestamp >> 5) & 0x3FU);
    datetime->second = (uint8_t)((timestamp & 0x1FU) * 2U);
}

int test_fatfs_timestamp(
    mtfs_test_t *test,
    const char *volume_path,
    mtfs_fatfs_timestamp_result_t *timestamp_result)
{
    static const BYTE payload = 0xA5U;
    char test_path[MTFS_TIMESTAMP_PATH_SIZE];
    FATFS filesystem;
    FIL file;
    FILINFO information;
    mtfs_time_status_t status = MTFS_TIME_STATUS_UNAVAILABLE;
    FRESULT fat_result;
    UINT transferred = 0U;
    int mounted = 0;
    int file_open = 0;
    int file_created = 0;
    int result = 1;

    if ((test == NULL) || (timestamp_result == NULL)) {
        return result;
    }
    memset(timestamp_result, 0, sizeof(*timestamp_result));
    memset(&information, 0, sizeof(information));

    if (!MTFS_TEST_CHECK(test,
            mtfs_timestamp_make_path(
                test_path, sizeof(test_path), volume_path),
            "build timestamp test path from the selected volume")) {
        return result;
    }
    if (!MTFS_TEST_CHECK(test,
            (mtfs_time_get_local(&timestamp_result->rtc_before, &status) ==
                MTFS_OK) &&
                (status == MTFS_TIME_STATUS_VALID) &&
                (mtfs_datetime_to_fat(&timestamp_result->rtc_before,
                    &timestamp_result->fat_before) == MTFS_OK),
            "RTC provider is VALID before creating the file")) {
        return result;
    }

    fat_result = f_mount(&filesystem, volume_path, 1U);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK,
            "mount prepared FAT volume for timestamp test")) {
        goto cleanup;
    }
    mounted = 1;

    fat_result = f_unlink(test_path);
    if (!MTFS_TEST_CHECK(test,
            (fat_result == FR_OK) || (fat_result == FR_NO_FILE),
            "remove only a stale dedicated timestamp test file")) {
        goto cleanup;
    }
    fat_result = f_open(&file, test_path, FA_CREATE_NEW | FA_WRITE);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK,
            "create dedicated timestamp test file")) {
        goto cleanup;
    }
    file_open = 1;
    file_created = 1;
    fat_result = f_write(&file, &payload, 1U, &transferred);
    if (!MTFS_TEST_CHECK(test,
            (fat_result == FR_OK) && (transferred == 1U),
            "write timestamp test payload")) {
        goto cleanup;
    }
    fat_result = f_close(&file);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK,
            "close and timestamp the test file")) {
        goto cleanup;
    }
    file_open = 0;

    fat_result = f_stat(test_path, &information);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK,
            "read timestamp metadata with f_stat")) {
        goto cleanup;
    }
    timestamp_result->fat_file =
        ((uint32_t)information.fdate << 16) | information.ftime;
    mtfs_timestamp_decode(
        timestamp_result->fat_file, &timestamp_result->file_time);

    status = MTFS_TIME_STATUS_UNAVAILABLE;
    if (!MTFS_TEST_CHECK(test,
            (mtfs_time_get_local(&timestamp_result->rtc_after, &status) ==
                MTFS_OK) &&
                (status == MTFS_TIME_STATUS_VALID) &&
                (mtfs_datetime_to_fat(&timestamp_result->rtc_after,
                    &timestamp_result->fat_after) == MTFS_OK),
            "RTC provider remains VALID after creating the file")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(test,
            (timestamp_result->fat_file >= timestamp_result->fat_before) &&
                (timestamp_result->fat_file <= timestamp_result->fat_after),
            "file timestamp is within the RTC interval at FAT 2-second precision")) {
        goto cleanup;
    }

    fat_result = f_unlink(test_path);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK,
            "remove dedicated timestamp test file")) {
        goto cleanup;
    }
    file_created = 0;
    fat_result = f_mount(NULL, volume_path, 0U);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK,
            "unmount after timestamp test")) {
        goto cleanup;
    }
    mounted = 0;
    result = 0;

cleanup:
    if (file_open) {
        (void)f_close(&file);
    }
    if (file_created) {
        (void)f_unlink(test_path);
    }
    if (mounted) {
        (void)f_mount(NULL, volume_path, 0U);
    }
    return result;
}
