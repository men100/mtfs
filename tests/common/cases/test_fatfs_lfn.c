#include "test_fatfs_lfn.h"

#include <stddef.h>
#include <string.h>

#include "ff.h"

#define MTFS_LFN_PATH_SIZE (320U)

#if MTFS_FF_ENABLE_LFN
static int mtfs_lfn_make_path(char *path, size_t path_size,
    const char *volume_path, const char *name)
{
    size_t volume_length;
    size_t name_length;

    if ((path == NULL) || (volume_path == NULL) || (name == NULL)) {
        return 0;
    }
    volume_length = strlen(volume_path);
    name_length = strlen(name);
    if ((volume_length >= path_size) ||
        (name_length >= path_size - volume_length)) {
        return 0;
    }
    memcpy(path, volume_path, volume_length);
    memcpy(path + volume_length, name, name_length + 1U);
    return 1;
}

static int mtfs_lfn_path_is_unused(mtfs_test_t *test, const char *path)
{
    FILINFO info;
    FRESULT result = f_stat(path, &info);

    return MTFS_TEST_CHECK(test,
        (result == FR_NO_FILE) || (result == FR_NO_PATH),
        "refuse to overwrite a pre-existing Phase 3.6 test object");
}

static FRESULT mtfs_lfn_write_file(const char *path,
    const BYTE *data, UINT data_size)
{
    FIL file;
    FRESULT result;
    UINT transferred = 0U;

    result = f_open(&file, path, FA_CREATE_NEW | FA_WRITE);
    if (result != FR_OK) {
        return result;
    }
    result = f_write(&file, data, data_size, &transferred);
    if ((result == FR_OK) && (transferred != data_size)) {
        result = FR_DISK_ERR;
    }
    if (result == FR_OK) {
        result = f_sync(&file);
    }
    {
        FRESULT close_result = f_close(&file);
        if (result == FR_OK) {
            result = close_result;
        }
    }
    return result;
}

static int mtfs_lfn_read_matches(mtfs_test_t *test, const char *path,
    const BYTE *expected, UINT expected_size, const char *message)
{
    BYTE actual[96];
    FIL file;
    FRESULT result;
    UINT transferred = 0U;
    int matches;

    if (expected_size > (UINT)sizeof(actual)) {
        return MTFS_TEST_CHECK(test, 0, message);
    }
    result = f_open(&file, path, FA_READ);
    if (!MTFS_TEST_CHECK(test, result == FR_OK,
            "open long-name file for readback")) {
        return 0;
    }
    memset(actual, 0, sizeof(actual));
    result = f_read(&file, actual, (UINT)sizeof(actual), &transferred);
    matches = (result == FR_OK) && (transferred == expected_size) &&
        (memcmp(actual, expected, expected_size) == 0);
    (void)MTFS_TEST_CHECK(test, matches, message);
    (void)MTFS_TEST_CHECK(test, f_close(&file) == FR_OK,
        "close long-name file after readback");
    return matches;
}

static int mtfs_lfn_directory_contains(mtfs_test_t *test,
    const char *directory_path, const char *name)
{
    DIR directory;
    FILINFO info;
    FRESULT result;
    int found = 0;

    result = f_opendir(&directory, directory_path);
    if (!MTFS_TEST_CHECK(test, result == FR_OK,
            "open directory for long-name enumeration")) {
        return 0;
    }
    for (;;) {
        result = f_readdir(&directory, &info);
        if ((result != FR_OK) || (info.fname[0] == '\0')) {
            break;
        }
        if (strcmp(info.fname, name) == 0) {
            found = 1;
            break;
        }
    }
    (void)MTFS_TEST_CHECK(test, result == FR_OK,
        "enumerate directory containing long names");
    (void)MTFS_TEST_CHECK(test, found,
        "f_readdir returns the public long filename");
    (void)MTFS_TEST_CHECK(test, f_closedir(&directory) == FR_OK,
        "close enumerated directory");
    return found && (result == FR_OK);
}

static void mtfs_lfn_make_boundary_name(char *name, size_t length)
{
    static const char suffix[] = ".bin";
    size_t index;

    for (index = 0U; index < length - (sizeof(suffix) - 1U); ++index) {
        name[index] = (char)('a' + (index % 26U));
    }
    if (length >= 8U) {
        memcpy(name, "mtfs", 4U);
    }
    memcpy(name + length - (sizeof(suffix) - 1U), suffix, sizeof(suffix));
}
#endif

int test_fatfs_lfn(mtfs_test_t *test, const char *volume_path)
{
#if !MTFS_FF_ENABLE_LFN
    (void)volume_path;
    return MTFS_TEST_CHECK(test, FF_USE_LFN == 0,
        "repository LFN-disabled configuration remains available") ? 0 : 1;
#else
    static const char primary_name[] =
        "MicroTFS Inference_Result 2026-08-16.txt";
    static const char renamed_name[] =
        "MicroTFS Inference_Result 2026-08-16 renamed.txt";
    static const char alpha_name[] =
        "storage diagnostics baseline alpha.bin";
    static const char beta_name[] =
        "storage diagnostics baseline beta.bin";
    static const char directory_name[] =
        "microtfs phase36 long directory";
    static const char child_name[] =
        "microtfs phase36 long directory/long child result.bin";
    static const char child_leaf[] = "long child result.bin";
    static const BYTE primary_data[] = "microT-FS Phase 3.6 LFN payload";
    static const BYTE alpha_data[] = "alpha payload";
    static const BYTE beta_data[] = "beta payload";
    static const BYTE child_data[] = "nested payload";
    char primary[MTFS_LFN_PATH_SIZE];
    char renamed[MTFS_LFN_PATH_SIZE];
    char alpha[MTFS_LFN_PATH_SIZE];
    char beta[MTFS_LFN_PATH_SIZE];
    char directory[MTFS_LFN_PATH_SIZE];
    char child[MTFS_LFN_PATH_SIZE];
    char boundary[MTFS_LFN_PATH_SIZE];
    char overlong[MTFS_LFN_PATH_SIZE];
    char invalid[MTFS_LFN_PATH_SIZE];
    char boundary_name[FF_MAX_LFN + 1U];
    char overlong_name[FF_MAX_LFN + 2U];
    FATFS filesystem;
    FIL file;
    FILINFO info;
    FRESULT result;
    int mounted = 0;
    int file_open = 0;
    int primary_created = 0;
    int renamed_created = 0;
    int alpha_created = 0;
    int beta_created = 0;
    int directory_created = 0;
    int child_created = 0;
    int boundary_created = 0;
    int test_result = 1;

    if (!MTFS_TEST_CHECK(test,
            (FF_USE_LFN == 2) && (FF_LFN_UNICODE == 0) &&
                (FF_MAX_LFN == MTFS_FF_MAX_LFN),
            "use stack-based LFN buffers with the configured maximum")) {
        return test_result;
    }
    mtfs_lfn_make_boundary_name(boundary_name, FF_MAX_LFN);
    mtfs_lfn_make_boundary_name(overlong_name, FF_MAX_LFN + 1U);
    if (!MTFS_TEST_CHECK(test,
            mtfs_lfn_make_path(primary, sizeof(primary), volume_path,
                primary_name) &&
            mtfs_lfn_make_path(renamed, sizeof(renamed), volume_path,
                renamed_name) &&
            mtfs_lfn_make_path(alpha, sizeof(alpha), volume_path, alpha_name) &&
            mtfs_lfn_make_path(beta, sizeof(beta), volume_path, beta_name) &&
            mtfs_lfn_make_path(directory, sizeof(directory), volume_path,
                directory_name) &&
            mtfs_lfn_make_path(child, sizeof(child), volume_path, child_name) &&
            mtfs_lfn_make_path(boundary, sizeof(boundary), volume_path,
                boundary_name) &&
            mtfs_lfn_make_path(overlong, sizeof(overlong), volume_path,
                overlong_name) &&
            mtfs_lfn_make_path(invalid, sizeof(invalid), volume_path,
                "microtfs phase36 invalid?.bin"),
            "construct bounded Phase 3.6 test paths")) {
        return test_result;
    }

    result = f_mount(&filesystem, volume_path, 1U);
    if (!MTFS_TEST_CHECK(test, result == FR_OK,
            "mount prepared FAT volume for LFN tests")) {
        return test_result;
    }
    mounted = 1;
    if (!mtfs_lfn_path_is_unused(test, primary) ||
        !mtfs_lfn_path_is_unused(test, renamed) ||
        !mtfs_lfn_path_is_unused(test, alpha) ||
        !mtfs_lfn_path_is_unused(test, beta) ||
        !mtfs_lfn_path_is_unused(test, directory) ||
        !mtfs_lfn_path_is_unused(test, boundary)) {
        goto cleanup;
    }

    result = mtfs_lfn_write_file(primary, primary_data,
        (UINT)(sizeof(primary_data) - 1U));
    primary_created = (result == FR_OK);
    if (!MTFS_TEST_CHECK(test, result == FR_OK,
            "create, write, sync, and close an ASCII long filename")) {
        goto cleanup;
    }
    result = f_stat(primary, &info);
    if (!MTFS_TEST_CHECK(test,
            (result == FR_OK) &&
                (info.fsize == (FSIZE_t)(sizeof(primary_data) - 1U)),
            "f_stat reports the long-name file")) {
        goto cleanup;
    }
    result = f_rename(primary, renamed);
    if (result == FR_OK) {
        primary_created = 0;
        renamed_created = 1;
    }
    if (!MTFS_TEST_CHECK(test, result == FR_OK,
            "rename a long-name file")) {
        goto cleanup;
    }
    if (!mtfs_lfn_directory_contains(test, volume_path, renamed_name)) {
        goto cleanup;
    }

    result = f_mount(NULL, volume_path, 0U);
    if (!MTFS_TEST_CHECK(test, result == FR_OK,
            "unmount after creating a long-name file")) {
        goto cleanup;
    }
    mounted = 0;
    result = f_mount(&filesystem, volume_path, 1U);
    if (!MTFS_TEST_CHECK(test, result == FR_OK,
            "remount before long-name persistence verification")) {
        goto cleanup;
    }
    mounted = 1;
    if (!mtfs_lfn_read_matches(test, renamed, primary_data,
            (UINT)(sizeof(primary_data) - 1U),
            "renamed long-name payload persists across remount")) {
        goto cleanup;
    }

    result = f_open(&file, renamed, FA_CREATE_NEW | FA_WRITE);
    if (result == FR_OK) {
        file_open = 1;
    }
    if (!MTFS_TEST_CHECK(test, result == FR_EXIST,
            "creating an existing long filename reports FR_EXIST")) {
        goto cleanup;
    }
    result = mtfs_lfn_write_file(alpha, alpha_data,
        (UINT)(sizeof(alpha_data) - 1U));
    alpha_created = (result == FR_OK);
    if (!MTFS_TEST_CHECK(test, result == FR_OK,
            "create first colliding-SFN-prefix long filename")) {
        goto cleanup;
    }
    result = mtfs_lfn_write_file(beta, beta_data,
        (UINT)(sizeof(beta_data) - 1U));
    beta_created = (result == FR_OK);
    if (!MTFS_TEST_CHECK(test, result == FR_OK,
            "create second colliding-SFN-prefix long filename")) {
        goto cleanup;
    }
    if (!mtfs_lfn_read_matches(test, alpha, alpha_data,
            (UINT)(sizeof(alpha_data) - 1U),
            "first public LFN resolves to its own payload") ||
        !mtfs_lfn_read_matches(test, beta, beta_data,
            (UINT)(sizeof(beta_data) - 1U),
            "second public LFN resolves to its own payload")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(test, f_rename(alpha, beta) == FR_EXIST,
            "rename to an existing LFN reports FR_EXIST")) {
        goto cleanup;
    }

    result = f_mkdir(directory);
    directory_created = (result == FR_OK);
    if (!MTFS_TEST_CHECK(test, result == FR_OK,
            "create a long directory name")) {
        goto cleanup;
    }
    result = mtfs_lfn_write_file(child, child_data,
        (UINT)(sizeof(child_data) - 1U));
    child_created = (result == FR_OK);
    if (!MTFS_TEST_CHECK(test, result == FR_OK,
            "create a long-name file below a long directory")) {
        goto cleanup;
    }
    if (!mtfs_lfn_read_matches(test, child, child_data,
            (UINT)(sizeof(child_data) - 1U),
            "nested long-name payload matches") ||
        !mtfs_lfn_directory_contains(test, directory, child_leaf)) {
        goto cleanup;
    }

    result = f_open(&file, boundary, FA_CREATE_NEW | FA_WRITE);
    if (result == FR_OK) {
        boundary_created = 1;
        file_open = 1;
        result = f_close(&file);
        file_open = 0;
    }
    if (!MTFS_TEST_CHECK(test, result == FR_OK,
            "accept a filename exactly at MTFS_FF_MAX_LFN")) {
        goto cleanup;
    }
    result = f_open(&file, overlong, FA_CREATE_NEW | FA_WRITE);
    if (result == FR_OK) {
        (void)f_close(&file);
        (void)f_unlink(overlong);
    }
    if (!MTFS_TEST_CHECK(test, result == FR_INVALID_NAME,
            "reject a filename beyond MTFS_FF_MAX_LFN")) {
        goto cleanup;
    }
    result = f_open(&file, "", FA_CREATE_NEW | FA_WRITE);
    if (result == FR_OK) {
        (void)f_close(&file);
    }
    if (!MTFS_TEST_CHECK(test, result == FR_INVALID_NAME,
            "reject an empty filename")) {
        goto cleanup;
    }
    result = f_open(&file, invalid, FA_CREATE_NEW | FA_WRITE);
    if (result == FR_OK) {
        (void)f_close(&file);
        (void)f_unlink(invalid);
    }
    if (!MTFS_TEST_CHECK(test, result == FR_INVALID_NAME,
            "reject a FAT-forbidden filename character")) {
        goto cleanup;
    }

    test_result = 0;

cleanup:
    if (file_open) {
        (void)f_close(&file);
    }
    if (mounted) {
        if (child_created) (void)f_unlink(child);
        if (directory_created) (void)f_unlink(directory);
        if (boundary_created) (void)f_unlink(boundary);
        if (beta_created) (void)f_unlink(beta);
        if (alpha_created) (void)f_unlink(alpha);
        if (renamed_created) (void)f_unlink(renamed);
        if (primary_created) (void)f_unlink(primary);
        (void)MTFS_TEST_CHECK(test, f_mount(NULL, volume_path, 0U) == FR_OK,
            "unmount after LFN test cleanup");
    }
    return test_result;
#endif
}
