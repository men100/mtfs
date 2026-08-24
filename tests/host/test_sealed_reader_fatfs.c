#include "test_sealed_reader_fatfs.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "mtfs_sealed_reader_fatfs.h"

typedef struct media_fixture
{
    mtfs_error_t status;
    unsigned int calls;
} media_fixture_t;

static mtfs_error_t media_status(void *opaque)
{
    media_fixture_t *fixture = (media_fixture_t *)opaque;
    ++fixture->calls;
    return fixture->status;
}

int test_sealed_reader_fatfs(mtfs_test_t *test, const char *volume_path)
{
    static const uint8_t contents[] = {
        0x10U, 0x21U, 0x32U, 0x43U, 0x54U, 0x65U, 0x76U, 0x87U,
        0x98U, 0xa9U, 0xbaU, 0xcbU, 0xdcU, 0xedU, 0xfeU
    };
    char path[32];
    char missing[32];
    FATFS filesystem;
    FIL file;
    mtfs_sealed_reader_fatfs_t context;
    mtfs_sealed_reader_t reader;
    media_fixture_t media = {MTFS_OK, 0U};
    uint8_t buffer[sizeof(contents)];
    UINT transferred = 0U;
    size_t read_size = 99U;
    uint64_t size = 0U;
    FRESULT fat_result;
    int mounted = 0;
    int file_open = 0;

    (void)snprintf(path, sizeof(path), "%s/SEALED.BIN", volume_path);
    (void)snprintf(missing, sizeof(missing), "%s/MISSING.BIN", volume_path);
    fat_result = f_mount(&filesystem, volume_path, 1U);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK,
            "sealed reader mounts host volume"))
        return 1;
    mounted = 1;
    fat_result = f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK,
            "sealed reader fixture opens"))
        goto cleanup;
    file_open = 1;
    fat_result = f_write(&file, contents, (UINT)sizeof(contents), &transferred);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK &&
            transferred == (UINT)sizeof(contents),
            "sealed reader fixture writes exact bytes"))
        goto cleanup;
    (void)f_close(&file);
    file_open = 0;

    mtfs_sealed_reader_fatfs_init(&context);
    memset(&reader, 0xa5, sizeof(reader));
    if (!MTFS_TEST_CHECK(test,
            mtfs_sealed_reader_fatfs_open(&context, missing, media_status,
                &media, &reader) == MTFS_ERROR_NOT_FOUND &&
            context.open == 0U && reader.get_size == NULL,
            "sealed reader reports missing file and cleans failure"))
        goto cleanup;
    if (!MTFS_TEST_CHECK(test,
            mtfs_sealed_reader_fatfs_open(&context, path, media_status,
                &media, &reader) == MTFS_OK,
            "sealed reader opens read-only"))
        goto cleanup;
    if (!MTFS_TEST_CHECK(test,
            reader.get_size(reader.context, &size) == MTFS_OK &&
            size == sizeof(contents),
            "sealed reader reports exact size"))
        goto cleanup_reader;
    memset(buffer, 0, sizeof(buffer));
    if (!MTFS_TEST_CHECK(test,
            reader.read_at(reader.context, 0U, buffer, sizeof(buffer),
                &read_size) == MTFS_OK && read_size == sizeof(buffer) &&
            memcmp(buffer, contents, sizeof(buffer)) == 0,
            "sealed reader exact read"))
        goto cleanup_reader;
    memset(buffer, 0, sizeof(buffer));
    if (!MTFS_TEST_CHECK(test,
            reader.read_at(reader.context, 5U, buffer, 6U, &read_size) ==
                MTFS_OK && read_size == 6U &&
            memcmp(buffer, contents + 5U, 6U) == 0,
            "sealed reader random offset read"))
        goto cleanup_reader;
    if (!MTFS_TEST_CHECK(test,
            reader.read_at(reader.context, sizeof(contents), NULL, 0U,
                &read_size) == MTFS_OK && read_size == 0U &&
            reader.read_at(reader.context, sizeof(contents), buffer, 1U,
                &read_size) == MTFS_ERROR_IO && read_size == 0U,
            "sealed reader enforces EOF boundary"))
        goto cleanup_reader;
    if (sizeof(size_t) > sizeof(UINT)) {
        if (!MTFS_TEST_CHECK(test,
                reader.read_at(reader.context, 0U, buffer,
                    (size_t)UINT_MAX + 1U, &read_size) == MTFS_ERROR_OVERFLOW,
                "sealed reader rejects UINT conversion overflow"))
            goto cleanup_reader;
    }
    if (!MTFS_TEST_CHECK(test,
            reader.read_at(reader.context, UINT64_MAX, buffer, 1U,
                &read_size) == MTFS_ERROR_OVERFLOW && read_size == 0U,
            "sealed reader rejects offset arithmetic overflow"))
        goto cleanup_reader;
    media.status = MTFS_ERROR_NO_MEDIA;
    if (!MTFS_TEST_CHECK(test,
            reader.read_at(reader.context, 0U, buffer, 1U, &read_size) ==
                MTFS_ERROR_NO_MEDIA && read_size == 0U,
            "sealed reader maps stable absent media"))
        goto cleanup_reader;
    media.status = MTFS_ERROR_NOT_READY;
    if (!MTFS_TEST_CHECK(test,
            reader.get_size(reader.context, &size) == MTFS_ERROR_NOT_READY,
            "sealed reader maps media not ready"))
        goto cleanup_reader;
    media.status = MTFS_OK;

    /* Test-only fault injection: retain the adapter's captured size while
     * making its caller-owned FIL observe an early EOF. */
    context.file.obj.objsize = 4U;
    if (!MTFS_TEST_CHECK(test,
            reader.read_at(reader.context, 0U, buffer, sizeof(contents),
                &read_size) == MTFS_ERROR_IO && read_size == 4U,
            "sealed reader maps successful FatFs short read to IO"))
        goto cleanup_reader;

cleanup_reader:
    (void)MTFS_TEST_CHECK(test,
        mtfs_sealed_reader_fatfs_close(&context) == MTFS_OK,
        "sealed reader closes after success or failure");
    (void)MTFS_TEST_CHECK(test,
        mtfs_sealed_reader_fatfs_close(&context) == MTFS_OK,
        "sealed reader close is idempotent");
cleanup:
    if (file_open)
        (void)f_close(&file);
    (void)f_unlink(path);
    if (mounted)
        (void)f_mount(NULL, volume_path, 0U);
    return test->failures == 0U ? 0 : 1;
}
