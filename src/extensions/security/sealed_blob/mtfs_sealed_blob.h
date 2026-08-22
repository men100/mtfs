#ifndef MTFS_SEALED_BLOB_H
#define MTFS_SEALED_BLOB_H

#include "mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL
#include <stddef.h>
#include <stdint.h>
#include "mtfs_crypto_provider.h"
#include "mtfs_sealed_format.h"
#include "mtfs_sealed_reader.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_SEALED_BLOB_API_VERSION (1U)
#define MTFS_SEALED_WORK_API_VERSION (1U)

typedef enum mtfs_sealed_blob_state
{
    MTFS_SEALED_BLOB_CLOSED = 0,
    MTFS_SEALED_BLOB_OPENING = 1,
    MTFS_SEALED_BLOB_OPEN = 2,
    MTFS_SEALED_BLOB_LOADING = 3,
    MTFS_SEALED_BLOB_LOADED = 4,
    MTFS_SEALED_BLOB_ERROR = 5
} mtfs_sealed_blob_state_t;

typedef struct mtfs_sealed_work
{
    uint32_t api_version;
    uint32_t struct_size;
    uint8_t *manifest;
    size_t manifest_capacity;
    uint8_t *aad;
    size_t aad_capacity;
    uint8_t *ciphertext;
    size_t ciphertext_capacity;
    uint8_t *plaintext;
    size_t plaintext_capacity;
} mtfs_sealed_work_t;

typedef struct mtfs_sealed_blob
{
    uint32_t api_version;
    uint32_t struct_size;
    mtfs_sealed_blob_state_t state;
    mtfs_sealed_reader_t reader;
    mtfs_crypto_provider_t provider;
    mtfs_sealed_work_t work;
    mtfs_sealed_package_info_t info;
    mtfs_sealed_layout_t layout;
    mtfs_crypto_key_handle_t fleet_handle;
    mtfs_crypto_key_handle_t model_handle;
    uint64_t file_size;
} mtfs_sealed_blob_t;

void mtfs_sealed_blob_init(mtfs_sealed_blob_t *blob);
mtfs_error_t mtfs_sealed_blob_open(mtfs_sealed_blob_t *blob,
    const mtfs_sealed_reader_t *reader, const mtfs_crypto_provider_t *provider,
    const mtfs_sealed_work_t *work, mtfs_sealed_package_info_t *authenticated_info);
mtfs_error_t mtfs_sealed_blob_load(mtfs_sealed_blob_t *blob,
    void *destination, size_t destination_size, size_t *loaded_size);
mtfs_error_t mtfs_sealed_blob_close(mtfs_sealed_blob_t *blob);

#ifdef __cplusplus
}
#endif
#endif /* MTFS_ENABLE_SEALED_MODEL */

#endif /* MTFS_SEALED_BLOB_H */
