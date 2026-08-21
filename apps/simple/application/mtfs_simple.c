#include "mtfs_simple.h"

#include <string.h>

#include <tm/tmonitor.h>

#include "ff.h"

#define SIMPLE_VOLUME "0:"
#define SIMPLE_PATH   "0:/HELLO.TXT"

static const char simple_message[] = "Hello from microT-FS!\r\n";

int mtfs_simple_run(void)
{
    FATFS filesystem;
    FIL file;
    char read_buffer[sizeof(simple_message)];
    FRESULT fatfs_result;
    UINT transferred = 0U;
    int mounted = 0;
    int opened = 0;
    int created = 0;
    int failed = 1;

    tm_printf((UB *)"[simple] mount %s\n", (UB *)SIMPLE_VOLUME);
    fatfs_result = f_mount(&filesystem, SIMPLE_VOLUME, 1U);
    if (fatfs_result != FR_OK) {
        tm_printf((UB *)"[simple] mount FAIL fatfs=%u\n",
            (UW)fatfs_result);
        goto cleanup;
    }
    mounted = 1;

    fatfs_result = f_open(&file, SIMPLE_PATH,
        FA_CREATE_ALWAYS | FA_WRITE);
    if (fatfs_result != FR_OK) {
        tm_printf((UB *)"[simple] create FAIL fatfs=%u\n",
            (UW)fatfs_result);
        goto cleanup;
    }
    opened = 1;
    created = 1;

    fatfs_result = f_write(&file, simple_message,
        sizeof(simple_message) - 1U, &transferred);
    if ((fatfs_result != FR_OK) ||
        (transferred != sizeof(simple_message) - 1U)) {
        tm_printf((UB *)"[simple] write FAIL fatfs=%u bytes=%u\n",
            (UW)fatfs_result, transferred);
        goto cleanup;
    }
    tm_printf((UB *)"[simple] write PASS bytes=%u\n", transferred);

    fatfs_result = f_close(&file);
    opened = 0;
    if (fatfs_result != FR_OK) {
        tm_printf((UB *)"[simple] close-after-write FAIL fatfs=%u\n",
            (UW)fatfs_result);
        goto cleanup;
    }

    fatfs_result = f_open(&file, SIMPLE_PATH, FA_READ);
    if (fatfs_result != FR_OK) {
        tm_printf((UB *)"[simple] reopen FAIL fatfs=%u\n",
            (UW)fatfs_result);
        goto cleanup;
    }
    opened = 1;
    (void)memset(read_buffer, 0, sizeof(read_buffer));
    transferred = 0U;
    fatfs_result = f_read(&file, read_buffer, sizeof(read_buffer),
        &transferred);
    if ((fatfs_result != FR_OK) ||
        (transferred != sizeof(simple_message) - 1U) ||
        (memcmp(read_buffer, simple_message,
            sizeof(simple_message) - 1U) != 0)) {
        tm_printf((UB *)"[simple] read/compare FAIL fatfs=%u bytes=%u\n",
            (UW)fatfs_result, transferred);
        goto cleanup;
    }
    tm_printf((UB *)"[simple] read/compare PASS bytes=%u\n",
        transferred);

    fatfs_result = f_close(&file);
    opened = 0;
    if (fatfs_result != FR_OK) {
        tm_printf((UB *)"[simple] close-after-read FAIL fatfs=%u\n",
            (UW)fatfs_result);
        goto cleanup;
    }

    fatfs_result = f_unlink(SIMPLE_PATH);
    if (fatfs_result != FR_OK) {
        tm_printf((UB *)"[simple] delete FAIL fatfs=%u\n",
            (UW)fatfs_result);
        goto cleanup;
    }
    created = 0;
    tm_printf((UB *)"[simple] delete PASS path=%s\n",
        (UB *)SIMPLE_PATH);
    failed = 0;

cleanup:
    if (opened) {
        FRESULT close_result = f_close(&file);
        if (close_result != FR_OK) {
            tm_printf((UB *)"[simple] cleanup close FAIL fatfs=%u\n",
                (UW)close_result);
            failed = 1;
        }
    }
    if (mounted && created) {
        FRESULT delete_result = f_unlink(SIMPLE_PATH);
        if ((delete_result != FR_OK) && (delete_result != FR_NO_FILE)) {
            tm_printf((UB *)"[simple] cleanup delete FAIL fatfs=%u\n",
                (UW)delete_result);
            failed = 1;
        }
    }
    if (mounted) {
        FRESULT unmount_result = f_mount(NULL, SIMPLE_VOLUME, 0U);
        if (unmount_result != FR_OK) {
            tm_printf((UB *)"[simple] unmount FAIL fatfs=%u\n",
                (UW)unmount_result);
            failed = 1;
        } else {
            tm_printf((UB *)"[simple] unmount PASS\n");
        }
    }
    tm_printf((UB *)"[simple] overall=%s\n",
        failed ? (UB *)"FAIL" : (UB *)"PASS");
    return failed;
}
