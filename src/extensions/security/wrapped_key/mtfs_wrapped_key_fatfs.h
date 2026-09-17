/** @file mtfs_wrapped_key_fatfs.h
 * @brief FatFs storage adapter for wrapped-key records. / wrapped key recordをFatFs上に保存するためのstorage adapter。
 * @ingroup mtfs_fatfs */
#ifndef MTFS_WRAPPED_KEY_FATFS_H
#define MTFS_WRAPPED_KEY_FATFS_H

/** @addtogroup mtfs_fatfs
 * @{ */

#include "../../../mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL

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

/** @brief Load and validate an existing record without exposing a plaintext key. / plaintext keyを外部に公開せず、既存recordをloadして検証する。
 * @param path FatFs path. / FatFs path。
 * @param[out] record Caller buffer retained while returned blob is used. / 返されたblobを使用している間、有効な状態を維持する呼び出し側のbuffer。
 * @param record_capacity Buffer bytes. / record bufferの容量（byte単位）。
 * @param expected_provider Required provider id. / recordに必須のprovider id。
 * @param expected_key_type Required key type. / recordに必須のkey type。
 * @param expected_blob_bytes Exact blob size. / recordに必須のblobサイズ。
 * @param[out] metadata Decoded metadata. / decodeしたmetadata。
 * @param[out] blob Borrowed view into record. / record内部を参照する非所有のblob view。
 * @param[out] diagnostics Optional operation diagnostics; may be NULL. / 処理結果のdiagnostics出力。省略可能で、NULL指定可。
 * @return FatFs adapter status. / FatFs adapterのstatus。 */
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

/** @brief Atomically create a new wrapped-key record using a temporary path. / temporary pathを使用して、新しいwrapped key recordをatomicに作成する。
 * @param path Final path, which must not already exist. / 最終的な保存先path。既存fileが存在してはならない。
 * @param temporary_path Distinct temporary path. / pathとは異なるtemporary path。
 * @param metadata Record metadata. / record metadata。
 * @param blob Opaque wrapped-key bytes. / 内部内容を直接扱わないwrapped key data。
 * @param blob_bytes Blob size. / blobのサイズ（byte単位）。
 * @param record_work Caller work buffer. / 呼び出し側が用意するwork buffer。
 * @param record_work_bytes Work capacity. / work bufferの容量（byte単位）。
 * @param[out] diagnostics Optional diagnostics; may be NULL. / diagnostics出力。省略可能で、NULL指定可。
 * @return FatFs adapter status. / FatFs adapterのstatus。
 * @warning The API refuses replacement of an existing final record. / 既存のfinal recordを上書き・置換することはできない。 */
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

#endif /* MTFS_ENABLE_SEALED_MODEL */

/** @} */

#endif /* MTFS_WRAPPED_KEY_FATFS_H */
