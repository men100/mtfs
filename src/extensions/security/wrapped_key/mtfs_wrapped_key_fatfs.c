#include "../../../mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL

#include "mtfs_wrapped_key_fatfs.h"

#include <limits.h>
#include <string.h>

static void secure_zero(void *data, size_t data_bytes)
{
    volatile uint8_t *cursor = (volatile uint8_t *)data;

    while (data_bytes != 0U) {
        *cursor++ = 0U;
        --data_bytes;
    }
#if defined(__GNUC__)
    __asm volatile ("" : : "r" (data) : "memory");
#endif
}

static mtfs_wrapped_key_fatfs_status_t map_open_result(FRESULT result)
{
    if ((result == FR_NO_FILE) || (result == FR_NO_PATH)) {
        return MTFS_WRAPPED_KEY_FATFS_NOT_FOUND;
    }
    if (result == FR_EXIST) {
        return MTFS_WRAPPED_KEY_FATFS_ALREADY_EXISTS;
    }
    return MTFS_WRAPPED_KEY_FATFS_IO;
}

mtfs_wrapped_key_fatfs_status_t mtfs_wrapped_key_fatfs_load(
    const char *path,
    uint8_t *record,
    size_t record_capacity,
    uint32_t expected_provider,
    uint32_t expected_key_type,
    size_t expected_blob_bytes,
    mtfs_wrapped_key_metadata_t *metadata,
    const uint8_t **blob,
    mtfs_wrapped_key_fatfs_diagnostics_t *diagnostics)
{
    FIL file;
    UINT read_bytes = 0U;
    UINT extra_bytes = 0U;
    uint8_t extra = 0U;
    size_t required_bytes;
    FRESULT result;
    mtfs_wrapped_key_record_status_t record_status;

    if ((path == NULL) || (record == NULL) || (metadata == NULL) ||
        (blob == NULL) || (diagnostics == NULL)) {
        return MTFS_WRAPPED_KEY_FATFS_INVALID_ARGUMENT;
    }
    required_bytes = mtfs_wrapped_key_record_size(expected_blob_bytes);
    if ((required_bytes == 0U) || (record_capacity < required_bytes) ||
        (required_bytes > UINT_MAX)) {
        return MTFS_WRAPPED_KEY_FATFS_INVALID_ARGUMENT;
    }

    *blob = NULL;
    secure_zero(record, required_bytes);
    result = f_open(&file, path, FA_READ);
    diagnostics->last_fatfs_result = result;
    if (result != FR_OK) {
        return map_open_result(result);
    }
    result = f_read(&file, record, (UINT)required_bytes, &read_bytes);
    if ((result == FR_OK) && (read_bytes == required_bytes)) {
        result = f_read(&file, &extra, 1U, &extra_bytes);
    }
    if (f_close(&file) != FR_OK) {
        result = FR_DISK_ERR;
    }
    diagnostics->last_fatfs_result = result;
    ++diagnostics->read_count;
    if ((result != FR_OK) || (read_bytes != required_bytes)) {
        secure_zero(record, required_bytes);
        return MTFS_WRAPPED_KEY_FATFS_IO;
    }
    if (extra_bytes != 0U) {
        secure_zero(record, required_bytes);
        return MTFS_WRAPPED_KEY_FATFS_INVALID_RECORD;
    }

    record_status = mtfs_wrapped_key_record_decode(record, required_bytes,
        expected_provider, expected_key_type, expected_blob_bytes,
        metadata, blob);
    diagnostics->last_record_status = record_status;
    if (record_status != MTFS_WRAPPED_KEY_RECORD_OK) {
        secure_zero(record, required_bytes);
        *blob = NULL;
        return MTFS_WRAPPED_KEY_FATFS_INVALID_RECORD;
    }
    return MTFS_WRAPPED_KEY_FATFS_OK;
}

mtfs_wrapped_key_fatfs_status_t mtfs_wrapped_key_fatfs_create(
    const char *path,
    const char *temporary_path,
    const mtfs_wrapped_key_metadata_t *metadata,
    const uint8_t *blob,
    size_t blob_bytes,
    uint8_t *record_work,
    size_t record_work_bytes,
    mtfs_wrapped_key_fatfs_diagnostics_t *diagnostics)
{
    FIL file;
    FILINFO information;
    UINT written_bytes = 0U;
    size_t encoded_bytes = 0U;
    const uint8_t *verified_blob = NULL;
    mtfs_wrapped_key_metadata_t verified_metadata;
    mtfs_wrapped_key_record_status_t record_status;
    mtfs_wrapped_key_fatfs_status_t status;
    FRESULT result;

    if ((path == NULL) || (temporary_path == NULL) ||
        (metadata == NULL) || (blob == NULL) || (record_work == NULL) ||
        (diagnostics == NULL) || (strcmp(path, temporary_path) == 0)) {
        return MTFS_WRAPPED_KEY_FATFS_INVALID_ARGUMENT;
    }
    record_status = mtfs_wrapped_key_record_encode(record_work,
        record_work_bytes, metadata, blob, blob_bytes, &encoded_bytes);
    diagnostics->last_record_status = record_status;
    if (record_status != MTFS_WRAPPED_KEY_RECORD_OK) {
        return MTFS_WRAPPED_KEY_FATFS_INVALID_ARGUMENT;
    }

    result = f_stat(path, &information);
    diagnostics->last_fatfs_result = result;
    if (result == FR_OK) {
        secure_zero(record_work, encoded_bytes);
        return MTFS_WRAPPED_KEY_FATFS_ALREADY_EXISTS;
    }
    if ((result != FR_NO_FILE) && (result != FR_NO_PATH)) {
        secure_zero(record_work, encoded_bytes);
        return MTFS_WRAPPED_KEY_FATFS_IO;
    }
    result = f_stat(temporary_path, &information);
    diagnostics->last_fatfs_result = result;
    if (result == FR_OK) {
        secure_zero(record_work, encoded_bytes);
        return MTFS_WRAPPED_KEY_FATFS_ALREADY_EXISTS;
    }
    if ((result != FR_NO_FILE) && (result != FR_NO_PATH)) {
        secure_zero(record_work, encoded_bytes);
        return MTFS_WRAPPED_KEY_FATFS_IO;
    }

    result = f_open(&file, temporary_path, FA_WRITE | FA_CREATE_NEW);
    diagnostics->last_fatfs_result = result;
    if (result != FR_OK) {
        secure_zero(record_work, encoded_bytes);
        return map_open_result(result);
    }
    ++diagnostics->create_count;
    result = f_write(&file, record_work, (UINT)encoded_bytes,
        &written_bytes);
    if ((result == FR_OK) && (written_bytes == encoded_bytes)) {
        result = f_sync(&file);
        if (result == FR_OK) {
            ++diagnostics->sync_count;
        }
    }
    if (f_close(&file) != FR_OK) {
        result = FR_DISK_ERR;
    }
    diagnostics->last_fatfs_result = result;
    secure_zero(record_work, encoded_bytes);
    if ((result != FR_OK) || (written_bytes != encoded_bytes)) {
        return MTFS_WRAPPED_KEY_FATFS_IO;
    }

    status = mtfs_wrapped_key_fatfs_load(temporary_path, record_work,
        record_work_bytes, metadata->provider, metadata->key_type,
        blob_bytes, &verified_metadata, &verified_blob, diagnostics);
    if (status != MTFS_WRAPPED_KEY_FATFS_OK) {
        return status;
    }
    ++diagnostics->verify_count;
    if ((verified_metadata.key_id != metadata->key_id) ||
        (verified_metadata.key_version != metadata->key_version) ||
        (memcmp(verified_blob, blob, blob_bytes) != 0)) {
        secure_zero(record_work, encoded_bytes);
        return MTFS_WRAPPED_KEY_FATFS_INVALID_RECORD;
    }
    secure_zero(record_work, encoded_bytes);

    result = f_rename(temporary_path, path);
    diagnostics->last_fatfs_result = result;
    if (result != FR_OK) {
        return MTFS_WRAPPED_KEY_FATFS_IO;
    }
    return MTFS_WRAPPED_KEY_FATFS_OK;
}

const char *mtfs_wrapped_key_fatfs_status_string(
    mtfs_wrapped_key_fatfs_status_t status)
{
    switch (status) {
    case MTFS_WRAPPED_KEY_FATFS_OK:
        return "ok";
    case MTFS_WRAPPED_KEY_FATFS_INVALID_ARGUMENT:
        return "invalid-argument";
    case MTFS_WRAPPED_KEY_FATFS_NOT_FOUND:
        return "not-found";
    case MTFS_WRAPPED_KEY_FATFS_ALREADY_EXISTS:
        return "already-exists";
    case MTFS_WRAPPED_KEY_FATFS_IO:
        return "io";
    case MTFS_WRAPPED_KEY_FATFS_INVALID_RECORD:
        return "invalid-record";
    default:
        return "unknown";
    }
}

#else
typedef int mtfs_wrapped_key_fatfs_disabled_translation_unit_t;
#endif /* MTFS_ENABLE_SEALED_MODEL */
