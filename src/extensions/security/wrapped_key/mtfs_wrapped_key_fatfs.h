#ifndef MTFS_WRAPPED_KEY_FATFS_H
#define MTFS_WRAPPED_KEY_FATFS_H

#include <stddef.h>
#include <stdint.h>

#include "ff.h"
#include "mtfs_wrapped_key_record.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mtfs_wrapped_key_fatfs_status
{
    MTFS_WRAPPED_KEY_FATFS_OK = 0,
    MTFS_WRAPPED_KEY_FATFS_INVALID_ARGUMENT = -1,
    MTFS_WRAPPED_KEY_FATFS_NOT_FOUND = -2,
    MTFS_WRAPPED_KEY_FATFS_ALREADY_EXISTS = -3,
    MTFS_WRAPPED_KEY_FATFS_IO = -4,
    MTFS_WRAPPED_KEY_FATFS_INVALID_RECORD = -5
} mtfs_wrapped_key_fatfs_status_t;

typedef struct mtfs_wrapped_key_fatfs_diagnostics
{
    FRESULT last_fatfs_result;
    mtfs_wrapped_key_record_status_t last_record_status;
    uint32_t read_count;
    uint32_t create_count;
    uint32_t sync_count;
    uint32_t verify_count;
} mtfs_wrapped_key_fatfs_diagnostics_t;

mtfs_wrapped_key_fatfs_status_t mtfs_wrapped_key_fatfs_load(
    const char *path,
    uint8_t *record,
    size_t record_capacity,
    uint32_t expected_provider,
    uint32_t expected_key_type,
    size_t expected_blob_bytes,
    mtfs_wrapped_key_metadata_t *metadata,
    const uint8_t **blob,
    mtfs_wrapped_key_fatfs_diagnostics_t *diagnostics);

mtfs_wrapped_key_fatfs_status_t mtfs_wrapped_key_fatfs_create(
    const char *path,
    const char *temporary_path,
    const mtfs_wrapped_key_metadata_t *metadata,
    const uint8_t *blob,
    size_t blob_bytes,
    uint8_t *record_work,
    size_t record_work_bytes,
    mtfs_wrapped_key_fatfs_diagnostics_t *diagnostics);

const char *mtfs_wrapped_key_fatfs_status_string(
    mtfs_wrapped_key_fatfs_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_WRAPPED_KEY_FATFS_H */
