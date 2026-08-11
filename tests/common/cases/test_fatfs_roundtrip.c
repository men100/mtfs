#include "test_fatfs_roundtrip.h"

#include <string.h>

#include "ff.h"

#define MTFS_ROUNDTRIP_DATA_SIZE (1537U)

int test_fatfs_roundtrip(mtfs_test_t *test, const char *volume_path)
{
    static const char test_path[] = "0:RTTEST.BIN";
    FATFS filesystem;
    FIL file;
    BYTE expected[MTFS_ROUNDTRIP_DATA_SIZE];
    BYTE actual[MTFS_ROUNDTRIP_DATA_SIZE + 1U];
    UINT transferred = 0U;
    FRESULT fat_result;
    unsigned int i;
    int mounted = 0;
    int file_open = 0;
    int result = 1;

    for (i = 0U; i < MTFS_ROUNDTRIP_DATA_SIZE; ++i) {
        expected[i] = (BYTE)((i * 37U + 11U) & 0xFFU);
    }
    memset(actual, 0, sizeof(actual));

    fat_result = f_mount(&filesystem, volume_path, 1U);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK, "mount prepared FAT volume")) {
        goto cleanup;
    }
    mounted = 1;

    fat_result = f_open(&file, test_path, FA_CREATE_ALWAYS | FA_WRITE);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK, "create 8.3 round-trip file")) {
        goto cleanup;
    }
    file_open = 1;
    fat_result = f_write(&file, expected, (UINT)sizeof(expected), &transferred);
    if (!MTFS_TEST_CHECK(test,
            (fat_result == FR_OK) && (transferred == (UINT)sizeof(expected)),
            "write complete known payload")) {
        goto cleanup;
    }
    fat_result = f_close(&file);
    file_open = 0;
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK, "close written file")) {
        goto cleanup;
    }

    fat_result = f_mount(NULL, volume_path, 0U);
    mounted = 0;
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK, "unmount after write")) {
        goto cleanup;
    }
    fat_result = f_mount(&filesystem, volume_path, 1U);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK, "remount FAT volume")) {
        goto cleanup;
    }
    mounted = 1;

    fat_result = f_open(&file, test_path, FA_READ);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK, "open round-trip file for read")) {
        goto cleanup;
    }
    file_open = 1;
    if (!MTFS_TEST_CHECK(test, f_size(&file) == (FSIZE_t)sizeof(expected),
            "persisted file length matches")) {
        goto cleanup;
    }
    fat_result = f_read(&file, actual, (UINT)sizeof(actual), &transferred);
    if (!MTFS_TEST_CHECK(test,
            (fat_result == FR_OK) && (transferred == (UINT)sizeof(expected)),
            "read exact persisted payload length")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(test, memcmp(expected, actual, sizeof(expected)) == 0,
            "persisted payload content matches")) {
        goto cleanup;
    }
    fat_result = f_close(&file);
    file_open = 0;
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK, "close read file")) {
        goto cleanup;
    }
    fat_result = f_unlink(test_path);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK, "remove dedicated test file")) {
        goto cleanup;
    }
    fat_result = f_mount(NULL, volume_path, 0U);
    mounted = 0;
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK, "final unmount")) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (file_open) {
        (void)f_close(&file);
    }
    if (mounted) {
        (void)f_unlink(test_path);
        (void)f_mount(NULL, volume_path, 0U);
    }
    return result;
}
