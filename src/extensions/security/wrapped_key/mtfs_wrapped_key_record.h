#ifndef MTFS_WRAPPED_KEY_RECORD_H
#define MTFS_WRAPPED_KEY_RECORD_H

#include "../../../mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_WRAPPED_KEY_RECORD_FORMAT_VERSION (1U)
#define MTFS_WRAPPED_KEY_RECORD_HEADER_BYTES    (32U)
#define MTFS_WRAPPED_KEY_RECORD_RSIP_AES256_BLOB_BYTES (52U)
#define MTFS_WRAPPED_KEY_RECORD_RSIP_AES256_BYTES \
    (MTFS_WRAPPED_KEY_RECORD_HEADER_BYTES + \
     MTFS_WRAPPED_KEY_RECORD_RSIP_AES256_BLOB_BYTES)
#define MTFS_WRAPPED_KEY_RECORD_SAES_AES256_BLOB_BYTES (32U)
#define MTFS_WRAPPED_KEY_RECORD_SAES_AES256_BYTES \
    (MTFS_WRAPPED_KEY_RECORD_HEADER_BYTES + \
     MTFS_WRAPPED_KEY_RECORD_SAES_AES256_BLOB_BYTES)

typedef enum mtfs_wrapped_key_provider
{
    MTFS_WRAPPED_KEY_PROVIDER_RA_RSIP_E50D = 1,
    MTFS_WRAPPED_KEY_PROVIDER_STM32_SAES_DHUK = 2
} mtfs_wrapped_key_provider_t;

typedef enum mtfs_wrapped_key_type
{
    MTFS_WRAPPED_KEY_TYPE_AES_256 = 1
} mtfs_wrapped_key_type_t;

typedef enum mtfs_wrapped_key_record_status
{
    MTFS_WRAPPED_KEY_RECORD_OK = 0,
    MTFS_WRAPPED_KEY_RECORD_INVALID_ARGUMENT = -1,
    MTFS_WRAPPED_KEY_RECORD_BUFFER_TOO_SMALL = -2,
    MTFS_WRAPPED_KEY_RECORD_INVALID_FORMAT = -3,
    MTFS_WRAPPED_KEY_RECORD_UNSUPPORTED = -4,
    MTFS_WRAPPED_KEY_RECORD_CRC_MISMATCH = -5
} mtfs_wrapped_key_record_status_t;

typedef struct mtfs_wrapped_key_metadata
{
    uint32_t provider;
    uint32_t key_type;
    uint32_t key_id;
    uint32_t key_version;
} mtfs_wrapped_key_metadata_t;

uint32_t mtfs_wrapped_key_record_crc32(
    const void *data, size_t data_bytes);

size_t mtfs_wrapped_key_record_size(size_t blob_bytes);

mtfs_wrapped_key_record_status_t mtfs_wrapped_key_record_encode(
    uint8_t *record,
    size_t record_capacity,
    const mtfs_wrapped_key_metadata_t *metadata,
    const uint8_t *blob,
    size_t blob_bytes,
    size_t *record_bytes);

mtfs_wrapped_key_record_status_t mtfs_wrapped_key_record_decode(
    const uint8_t *record,
    size_t record_bytes,
    uint32_t expected_provider,
    uint32_t expected_key_type,
    size_t expected_blob_bytes,
    mtfs_wrapped_key_metadata_t *metadata,
    const uint8_t **blob);

const char *mtfs_wrapped_key_record_status_string(
    mtfs_wrapped_key_record_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_ENABLE_SEALED_MODEL */

#endif /* MTFS_WRAPPED_KEY_RECORD_H */
