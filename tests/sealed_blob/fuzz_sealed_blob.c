#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mtfs_sealed_blob.h"

typedef struct fuzz_reader
{
    const uint8_t *data;
    size_t size;
} fuzz_reader_t;

static mtfs_error_t fuzz_size(void *context, uint64_t *size)
{
    fuzz_reader_t *reader = (fuzz_reader_t *)context;
    *size = reader->size;
    return MTFS_OK;
}

static mtfs_error_t fuzz_read(void *context, uint64_t offset, void *buffer,
                              size_t requested, size_t *read_size)
{
    fuzz_reader_t *reader = (fuzz_reader_t *)context;
    *read_size = 0U;
    if (offset > reader->size || requested > reader->size - (size_t)offset)
        return MTFS_ERROR_IO;
    memcpy(buffer, reader->data + (size_t)offset, requested);
    *read_size = requested;
    return MTFS_OK;
}

static mtfs_crypto_status_t fuzz_open_fleet(void *context, uint32_t id,
    uint32_t version, mtfs_crypto_key_handle_t *handle)
{
    (void)context;
    if (id != 1U || version != 1U)
        return MTFS_CRYPTO_NOT_SUPPORTED;
    *handle = 1U;
    return MTFS_CRYPTO_OK;
}

static mtfs_crypto_status_t fuzz_open_model(void *context,
    mtfs_crypto_key_handle_t fleet, const uint8_t nonce[12],
    const uint8_t *aad, size_t aad_size, const uint8_t ciphertext[32],
    const uint8_t tag[16], mtfs_crypto_key_handle_t *handle)
{
    (void)context; (void)nonce; (void)aad; (void)aad_size;
    (void)ciphertext; (void)tag;
    if (fleet != 1U)
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    *handle = 2U;
    return MTFS_CRYPTO_OK;
}

static mtfs_crypto_status_t fuzz_decrypt(void *context,
    mtfs_crypto_key_handle_t model, const uint8_t nonce[12],
    const uint8_t *aad, size_t aad_size, const uint8_t *ciphertext,
    size_t ciphertext_size, const uint8_t tag[16], uint8_t *plaintext)
{
    (void)context; (void)nonce; (void)aad; (void)aad_size; (void)tag;
    if (model != 2U)
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    memcpy(plaintext, ciphertext, ciphertext_size);
    return MTFS_CRYPTO_OK;
}

static mtfs_crypto_status_t fuzz_close(void *context,
                                       mtfs_crypto_key_handle_t handle)
{
    (void)context; (void)handle;
    return MTFS_CRYPTO_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    mtfs_sealed_blob_t blob;
    mtfs_sealed_package_info_t info;
    fuzz_reader_t memory = {data, size};
    mtfs_sealed_reader_t reader = {
        MTFS_SEALED_READER_API_VERSION, sizeof(mtfs_sealed_reader_t),
        &memory, fuzz_size, fuzz_read};
    mtfs_crypto_provider_t provider = {
        MTFS_CRYPTO_PROVIDER_API_VERSION, sizeof(mtfs_crypto_provider_t), NULL,
        fuzz_open_fleet, fuzz_open_model, fuzz_decrypt, fuzz_close};
    uint8_t manifest[MTFS_SEALED_MAX_MANIFEST_SIZE];
    uint8_t aad[MTFS_SEALED_CHUNK_AAD_SIZE];
    uint8_t ciphertext[MTFS_SEALED_CIPHER_BUFFER_SIZE];
    uint8_t plaintext[MTFS_SEALED_MAX_CHUNK_SIZE];
    mtfs_sealed_work_t work = {
        MTFS_SEALED_WORK_API_VERSION, sizeof(mtfs_sealed_work_t),
        manifest, sizeof(manifest), aad, sizeof(aad), ciphertext,
        sizeof(ciphertext), plaintext, sizeof(plaintext)};
    mtfs_sealed_blob_init(&blob);
    (void)mtfs_sealed_blob_open(&blob, &reader, &provider, &work, &info);
    (void)mtfs_sealed_blob_close(&blob);
    return 0;
}
