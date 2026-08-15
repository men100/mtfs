#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ff.h"
#include "diskio.h"
#include "mtfs_block_registry.h"
#include "mtfs_host_block_file.h"
#include "mtfs_test.h"
#include "test_fatfs_concurrent.h"
#include "test_fatfs_roundtrip.h"
#include "test_fatfs_timestamp.h"
#include "test_media_lifecycle.h"
#include "test_rtc_set_app.h"
#include "test_time_provider.h"

#define MTFS_HOST_IMAGE_SIZE (16L * 1024L * 1024L)
#define MTFS_HOST_SECTOR_SIZE MTFS_HOST_BLOCK_FILE_DEFAULT_SECTOR_SIZE

static mtfs_error_t mtfs_host_time_get_local(
    void *context, mtfs_datetime_t *datetime, mtfs_time_status_t *status)
{
    *datetime = *(const mtfs_datetime_t *)context;
    *status = MTFS_TIME_STATUS_VALID;
    return MTFS_OK;
}

static mtfs_error_t mtfs_host_time_set_local(
    void *context, const mtfs_datetime_t *datetime)
{
    *(mtfs_datetime_t *)context = *datetime;
    return MTFS_OK;
}

static mtfs_error_t mtfs_host_time_get_status(
    void *context, mtfs_time_status_t *status)
{
    (void)context;
    *status = MTFS_TIME_STATUS_VALID;
    return MTFS_OK;
}

static mtfs_error_t mtfs_host_time_clear(void *context)
{
    (void)context;
    return MTFS_OK;
}

static const mtfs_time_provider_ops_t mtfs_host_time_ops = {
    .get_local = mtfs_host_time_get_local,
    .set_local = mtfs_host_time_set_local,
    .get_status = mtfs_host_time_get_status,
    .clear = mtfs_host_time_clear
};

static int mtfs_create_temp_image(char *path, size_t path_size)
{
    const char template_path[] = "/tmp/mtfs_roundtrip_XXXXXX";
    FILE *stream;
    int descriptor;

    if (path_size < sizeof(template_path)) {
        return 0;
    }
    memcpy(path, template_path, sizeof(template_path));
    descriptor = mkstemp(path);
    if (descriptor < 0) {
        return 0;
    }
    if (close(descriptor) != 0) {
        (void)remove(path);
        return 0;
    }

    stream = fopen(path, "r+b");
    if (stream == NULL) {
        (void)remove(path);
        return 0;
    }
    if ((fseek(stream, MTFS_HOST_IMAGE_SIZE - 1L, SEEK_SET) != 0) ||
        (fputc(0, stream) == EOF)) {
        (void)fclose(stream);
        (void)remove(path);
        return 0;
    }
    if (fclose(stream) != 0) {
        (void)remove(path);
        return 0;
    }
    return 1;
}

static void mtfs_host_reporter(
    void *context,
    mtfs_test_event_t event,
    const char *test_name,
    const char *file,
    int line,
    const char *message,
    unsigned int checks,
    unsigned int failures)
{
    (void)context;
    if (event == MTFS_TEST_EVENT_BEGIN) {
        printf("[ RUN  ] %s\n", test_name);
    } else if (event == MTFS_TEST_EVENT_CHECK_FAILED) {
        printf("[ FAIL ] %s:%d: %s\n", file, line, message);
    } else if (failures == 0U) {
        printf("[ PASS ] %s (%u checks)\n", test_name, checks);
    } else {
        printf("[ FAIL ] %s (%u failures, %u checks)\n",
            test_name, failures, checks);
    }
}

int main(void)
{
    char image_path[128];
    mtfs_test_t test;
    mtfs_host_block_file_t host_context;
    mtfs_block_device_t device;
    BYTE sector_buffer[MTFS_HOST_SECTOR_SIZE];
    BYTE format_buffer[4096];
    MKFS_PARM format_options = {FM_FAT, 0U, 0U, 0U, 0U};
    LBA_t sector_count = 0U;
    WORD sector_size = 0U;
    DWORD erase_block_size = 0U;
    LBA_t trim_range[2] = {0U, 1U};
    FRESULT fat_result;
    int image_created = 0;
    int port_open = 0;
    int registered = 0;
    int time_provider_registered = 0;
    int roundtrip_result = 1;
    int concurrent_result = 1;
    int timestamp_result_code = 1;
    mtfs_datetime_t host_datetime = {2026U, 8U, 14U, 21U, 30U, 58U};
    mtfs_time_provider_t host_time_provider = {
        .ops = &mtfs_host_time_ops,
        .context = &host_datetime,
        .lock = NULL,
        .unlock = NULL,
        .lock_context = NULL
    };
    mtfs_fatfs_timestamp_result_t timestamp_result;

    memset(&host_context, 0, sizeof(host_context));
    memset(&device, 0, sizeof(device));
    memset(sector_buffer, 0xA5, sizeof(sector_buffer));
    mtfs_test_begin(&test, "host block device and FatFs concurrency",
        mtfs_host_reporter, NULL);

    if (test_time_provider(&test) != 0) {
        goto cleanup;
    }
    if (test_rtc_set_app(&test) != 0) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(&test,
            mtfs_time_provider_register(&host_time_provider) == MTFS_OK,
            "register fixed host provider for FatFs timestamp tests")) {
        goto cleanup;
    }
    time_provider_registered = 1;

    if (test_media_lifecycle(&test) != 0) {
        goto cleanup;
    }

    if (!MTFS_TEST_CHECK(&test,
            mtfs_block_registry_register(0U, NULL) == MTFS_ERROR_INVALID_ARGUMENT,
            "reject NULL block device")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(&test,
            mtfs_block_registry_register(0U, &device) == MTFS_ERROR_INVALID_ARGUMENT,
            "reject structurally invalid block device")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(&test, disk_status(0U) == STA_NOINIT,
            "unregistered physical drive reports not initialized")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(&test, mtfs_create_temp_image(image_path, sizeof(image_path)),
            "create unique host-test image")) {
        goto cleanup;
    }
    image_created = 1;
    if (!MTFS_TEST_CHECK(&test,
            mtfs_host_block_file_open(&host_context, &device, image_path,
                MTFS_HOST_SECTOR_SIZE, 0) == MTFS_OK,
            "open explicit temporary image as a block device")) {
        goto cleanup;
    }
    port_open = 1;
    if (!MTFS_TEST_CHECK(&test,
            mtfs_block_registry_register(MTFS_BLOCK_REGISTRY_SIZE, &device) ==
                MTFS_ERROR_OUT_OF_RANGE,
            "reject out-of-range physical drive")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(&test,
            mtfs_block_registry_register(0U, &device) == MTFS_OK,
            "register physical drive")) {
        goto cleanup;
    }
    registered = 1;
    if (!MTFS_TEST_CHECK(&test, disk_status(0U) == STA_NOINIT,
            "registered drive remains uninitialized before initialize")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(&test,
            mtfs_block_registry_register(0U, &device) == MTFS_ERROR_ALREADY_EXISTS,
            "reject duplicate physical-drive registration")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(&test, disk_initialize(0U) == 0U,
            "initialize registered physical drive")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(&test,
            disk_ioctl(0U, GET_SECTOR_COUNT, &sector_count) == RES_OK &&
                sector_count == (LBA_t)(MTFS_HOST_IMAGE_SIZE / MTFS_HOST_SECTOR_SIZE),
            "report sector count without truncation")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(&test,
            disk_ioctl(0U, GET_SECTOR_SIZE, &sector_size) == RES_OK &&
                sector_size == MTFS_HOST_SECTOR_SIZE,
            "report 512-byte sectors")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(&test,
            disk_ioctl(0U, GET_BLOCK_SIZE, &erase_block_size) == RES_OK &&
                erase_block_size == 1U,
            "report erase block size in sectors")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(&test,
            disk_read(0U, sector_buffer, 0U, 0U) == RES_PARERR,
            "reject zero-sector read")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(&test,
            disk_read(0U, sector_buffer, sector_count, 1U) == RES_PARERR,
            "reject out-of-range LBA")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(&test, disk_ioctl(0U, CTRL_SYNC, NULL) == RES_OK,
            "sync host image")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(&test,
            disk_ioctl(0U, CTRL_TRIM, trim_range) == RES_PARERR,
            "report unsupported host trim")) {
        goto cleanup;
    }

    fat_result = f_mkfs("0:", &format_options, format_buffer, (UINT)sizeof(format_buffer));
    if (!MTFS_TEST_CHECK(&test, fat_result == FR_OK,
            "host runner explicitly formats temporary image")) {
        goto cleanup;
    }
    roundtrip_result = test_fatfs_roundtrip(&test, "0:");
    if (roundtrip_result != 0) {
        goto cleanup;
    }
    timestamp_result_code = test_fatfs_timestamp(
        &test, "0:", &timestamp_result);
    if (timestamp_result_code != 0) {
        goto cleanup;
    }
    concurrent_result = test_fatfs_concurrent(&test, "0:");
    if (concurrent_result != 0) {
        goto cleanup;
    }

    if (!MTFS_TEST_CHECK(&test,
            mtfs_block_registry_unregister(0U) == MTFS_OK,
            "unregister physical drive")) {
        goto cleanup;
    }
    registered = 0;
    if (!MTFS_TEST_CHECK(&test,
            mtfs_host_block_file_close(&host_context) == MTFS_OK,
            "close writable image")) {
        goto cleanup;
    }
    port_open = 0;

    if (!MTFS_TEST_CHECK(&test,
            mtfs_host_block_file_open(&host_context, &device, image_path,
                MTFS_HOST_SECTOR_SIZE, 1) == MTFS_OK,
            "reopen image read-only")) {
        goto cleanup;
    }
    port_open = 1;
    if (!MTFS_TEST_CHECK(&test,
            mtfs_block_registry_register(0U, &device) == MTFS_OK,
            "register read-only physical drive")) {
        goto cleanup;
    }
    registered = 1;
    if (!MTFS_TEST_CHECK(&test,
            (disk_initialize(0U) & STA_PROTECT) != 0U,
            "read-only drive reports write protection")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(&test,
            disk_write(0U, sector_buffer, 0U, 1U) == RES_WRPRT,
            "map read-only write to RES_WRPRT")) {
        goto cleanup;
    }

cleanup:
    if (registered) {
        if (!MTFS_TEST_CHECK(&test,
                mtfs_block_registry_unregister(0U) == MTFS_OK,
                "cleanup unregisters physical drive")) {
            registered = 0;
        } else {
            registered = 0;
        }
    }
    if (port_open) {
        (void)MTFS_TEST_CHECK(&test,
            mtfs_host_block_file_close(&host_context) == MTFS_OK,
            "cleanup closes host image");
        port_open = 0;
    }
    if (image_created) {
        (void)MTFS_TEST_CHECK(&test, remove(image_path) == 0,
            "delete only the temporary image created by this test");
        image_created = 0;
    }
    if (time_provider_registered) {
        (void)MTFS_TEST_CHECK(&test,
            mtfs_time_provider_unregister(&host_time_provider) == MTFS_OK,
            "unregister fixed host time provider");
        time_provider_registered = 0;
    }
    return mtfs_test_finish(&test);
}
