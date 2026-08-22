#include "mtfs_sealed_blob.h"

#if MTFS_ENABLE_SEALED_MODEL

#include <limits.h>
#include <string.h>
#include "mtfs_secure_zero.h"

static const uint8_t mtfs_key_domain[12] =
    {'M','T','F','S','-','K','E','Y','-','v','1',0};
static const uint8_t mtfs_chunk_domain[14] =
    {'M','T','F','S','-','C','H','U','N','K','-','v','1',0};

static void mtfs_put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static int mtfs_ranges_overlap(const void *a, size_t a_size,
                               const void *b, size_t b_size)
{
    uintptr_t a_start;
    uintptr_t b_start;
    uintptr_t a_end;
    uintptr_t b_end;
    if (a_size == 0U || b_size == 0U)
        return 0;
    if (a == NULL || b == NULL)
        return 1;
    a_start = (uintptr_t)a;
    b_start = (uintptr_t)b;
    if (a_size > UINTPTR_MAX - a_start || b_size > UINTPTR_MAX - b_start)
        return 1;
    a_end = a_start + a_size;
    b_end = b_start + b_size;
    return a_start < b_end && b_start < a_end;
}

static mtfs_error_t mtfs_provider_error(mtfs_crypto_status_t status)
{
    switch (status)
    {
    case MTFS_CRYPTO_OK:
        return MTFS_OK;
    case MTFS_CRYPTO_INVALID_ARGUMENT:
        return MTFS_ERROR_INVALID_ARGUMENT;
    case MTFS_CRYPTO_NOT_SUPPORTED:
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    case MTFS_CRYPTO_AUTHENTICATION_FAILED:
        return MTFS_ERROR_AUTHENTICATION;
    case MTFS_CRYPTO_KEY_NOT_FOUND:
        return MTFS_ERROR_NOT_FOUND;
    case MTFS_CRYPTO_RESOURCE_EXHAUSTED:
        return MTFS_ERROR_BUFFER_TOO_SMALL;
    case MTFS_CRYPTO_IO_FAILED:
        return MTFS_ERROR_IO;
    case MTFS_CRYPTO_FAILED:
    default:
        return MTFS_ERROR_CRYPTO;
    }
}

static mtfs_error_t mtfs_read_exact(mtfs_sealed_blob_t *blob, uint64_t offset,
                                    void *buffer, size_t size)
{
    size_t received = 0U;
    mtfs_error_t result;
    if (size != 0U && buffer == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    if ((uint64_t)size > UINT64_MAX - offset ||
        offset + (uint64_t)size > blob->file_size)
        return MTFS_ERROR_IO;
    result = blob->reader.read_at(blob->reader.context, offset, buffer, size, &received);
    if (result != MTFS_OK)
        return result == MTFS_ERROR_OVERFLOW ? result : MTFS_ERROR_IO;
    return received == size ? MTFS_OK : MTFS_ERROR_IO;
}

static void mtfs_zero_work(const mtfs_sealed_work_t *work)
{
    if (work == NULL)
        return;
    mtfs_secure_zero(work->manifest, work->manifest_capacity);
    mtfs_secure_zero(work->aad, work->aad_capacity);
    mtfs_secure_zero(work->ciphertext, work->ciphertext_capacity);
    mtfs_secure_zero(work->plaintext, work->plaintext_capacity);
}

static mtfs_error_t mtfs_validate_interfaces(const mtfs_sealed_blob_t *blob,
    const mtfs_sealed_reader_t *reader, const mtfs_crypto_provider_t *provider,
    const mtfs_sealed_work_t *work)
{
    if (blob == NULL || reader == NULL || provider == NULL || work == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (blob->api_version != MTFS_SEALED_BLOB_API_VERSION ||
        blob->struct_size < sizeof(*blob) || blob->state != MTFS_SEALED_BLOB_CLOSED)
        return MTFS_ERROR_INVALID_STATE;
    if (reader->api_version != MTFS_SEALED_READER_API_VERSION ||
        reader->struct_size < sizeof(*reader) || reader->get_size == NULL ||
        reader->read_at == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (provider->api_version != MTFS_CRYPTO_PROVIDER_API_VERSION ||
        provider->struct_size < sizeof(*provider) || provider->open_fleet_key == NULL ||
        provider->open_model_key == NULL || provider->decrypt_chunk == NULL ||
        provider->close_key == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (work->api_version != MTFS_SEALED_WORK_API_VERSION ||
        work->struct_size < sizeof(*work) || work->manifest == NULL || work->aad == NULL ||
        work->ciphertext == NULL || work->plaintext == NULL ||
        work->manifest_capacity < MTFS_SEALED_MAX_MANIFEST_SIZE ||
        work->aad_capacity < MTFS_SEALED_CHUNK_AAD_SIZE ||
        work->ciphertext_capacity < MTFS_SEALED_CIPHER_BUFFER_SIZE ||
        work->plaintext_capacity < MTFS_SEALED_MAX_CHUNK_SIZE)
        return MTFS_ERROR_BUFFER_TOO_SMALL;
    if (mtfs_ranges_overlap(work->manifest, work->manifest_capacity,
                            work->aad, work->aad_capacity) ||
        mtfs_ranges_overlap(work->manifest, work->manifest_capacity,
                            work->ciphertext, work->ciphertext_capacity) ||
        mtfs_ranges_overlap(work->manifest, work->manifest_capacity,
                            work->plaintext, work->plaintext_capacity) ||
        mtfs_ranges_overlap(work->aad, work->aad_capacity,
                            work->ciphertext, work->ciphertext_capacity) ||
        mtfs_ranges_overlap(work->aad, work->aad_capacity,
                            work->plaintext, work->plaintext_capacity) ||
        mtfs_ranges_overlap(work->ciphertext, work->ciphertext_capacity,
                            work->plaintext, work->plaintext_capacity))
        return MTFS_ERROR_INVALID_ARGUMENT;
    return MTFS_OK;
}

static void mtfs_close_handle(mtfs_sealed_blob_t *blob,
                              mtfs_crypto_key_handle_t *handle)
{
    if (*handle != MTFS_CRYPTO_INVALID_KEY_HANDLE)
    {
        (void)blob->provider.close_key(blob->provider.context, *handle);
        *handle = MTFS_CRYPTO_INVALID_KEY_HANDLE;
    }
}

static mtfs_error_t mtfs_open_fail(mtfs_sealed_blob_t *blob, mtfs_error_t result)
{
    mtfs_close_handle(blob, &blob->model_handle);
    mtfs_close_handle(blob, &blob->fleet_handle);
    mtfs_zero_work(&blob->work);
    memset(&blob->info, 0, sizeof(blob->info));
    memset(&blob->layout, 0, sizeof(blob->layout));
    blob->state = MTFS_SEALED_BLOB_ERROR;
    return result;
}

void mtfs_sealed_blob_init(mtfs_sealed_blob_t *blob)
{
    if (blob != NULL)
    {
        memset(blob, 0, sizeof(*blob));
        blob->api_version = MTFS_SEALED_BLOB_API_VERSION;
        blob->struct_size = (uint32_t)sizeof(*blob);
        blob->state = MTFS_SEALED_BLOB_CLOSED;
    }
}

mtfs_error_t mtfs_sealed_blob_open(mtfs_sealed_blob_t *blob,
    const mtfs_sealed_reader_t *reader, const mtfs_crypto_provider_t *provider,
    const mtfs_sealed_work_t *work, mtfs_sealed_package_info_t *authenticated_info)
{
    mtfs_sealed_package_info_t candidate_info;
    mtfs_sealed_layout_t candidate_layout;
    mtfs_crypto_status_t crypto_result;
    mtfs_error_t result = mtfs_validate_interfaces(blob, reader, provider, work);
    size_t aad_size;
    if (result != MTFS_OK || authenticated_info == NULL)
        return result != MTFS_OK ? result : MTFS_ERROR_INVALID_ARGUMENT;
    if (mtfs_ranges_overlap(authenticated_info, sizeof(*authenticated_info),
                            blob, sizeof(*blob)) ||
        mtfs_ranges_overlap(authenticated_info, sizeof(*authenticated_info),
                            work->manifest, work->manifest_capacity) ||
        mtfs_ranges_overlap(authenticated_info, sizeof(*authenticated_info),
                            work->aad, work->aad_capacity) ||
        mtfs_ranges_overlap(authenticated_info, sizeof(*authenticated_info),
                            work->ciphertext, work->ciphertext_capacity) ||
        mtfs_ranges_overlap(authenticated_info, sizeof(*authenticated_info),
                            work->plaintext, work->plaintext_capacity))
        return MTFS_ERROR_INVALID_ARGUMENT;
    blob->reader = *reader;
    blob->provider = *provider;
    blob->work = *work;
    blob->state = MTFS_SEALED_BLOB_OPENING;
    result = blob->reader.get_size(blob->reader.context, &blob->file_size);
    if (result != MTFS_OK)
        return mtfs_open_fail(blob, MTFS_ERROR_IO);
    if (blob->file_size < MTFS_SEALED_PREAMBLE_SIZE)
        return mtfs_open_fail(blob, MTFS_ERROR_MALFORMED_FORMAT);
    result = mtfs_read_exact(blob, 0U, blob->work.manifest,
                             MTFS_SEALED_PREAMBLE_SIZE);
    if (result != MTFS_OK)
        return mtfs_open_fail(blob, result);
    result = mtfs_sealed_format_parse(blob->work.manifest, &candidate_info,
                                      &candidate_layout);
    if (result != MTFS_OK)
        return mtfs_open_fail(blob, result);
    if (candidate_layout.expected_file_size != blob->file_size)
        return mtfs_open_fail(blob, MTFS_ERROR_MALFORMED_FORMAT);
    if (candidate_layout.metadata_size != 0U)
    {
        result = mtfs_read_exact(blob, MTFS_SEALED_PREAMBLE_SIZE,
            blob->work.manifest + MTFS_SEALED_PREAMBLE_SIZE,
            candidate_layout.metadata_size);
        if (result != MTFS_OK)
            return mtfs_open_fail(blob, result);
    }
    result = mtfs_sealed_metadata_validate(
        blob->work.manifest + MTFS_SEALED_PREAMBLE_SIZE,
        candidate_layout.metadata_size);
    if (result != MTFS_OK)
        return mtfs_open_fail(blob, result);
    result = mtfs_read_exact(blob, candidate_layout.envelope_offset,
                             blob->work.ciphertext, MTFS_SEALED_ENVELOPE_SIZE);
    if (result != MTFS_OK)
        return mtfs_open_fail(blob, result);
    memcpy(blob->work.aad, mtfs_key_domain, sizeof(mtfs_key_domain));
    memcpy(blob->work.aad + sizeof(mtfs_key_domain), blob->work.manifest,
           candidate_layout.manifest_size);
    aad_size = sizeof(mtfs_key_domain) + candidate_layout.manifest_size;
    crypto_result = blob->provider.open_fleet_key(blob->provider.context,
        candidate_layout.key_id, candidate_layout.key_version, &blob->fleet_handle);
    if (crypto_result != MTFS_CRYPTO_OK ||
        blob->fleet_handle == MTFS_CRYPTO_INVALID_KEY_HANDLE)
        return mtfs_open_fail(blob, mtfs_provider_error(crypto_result));
    blob->model_handle = MTFS_CRYPTO_INVALID_KEY_HANDLE;
    crypto_result = blob->provider.open_model_key(blob->provider.context,
        blob->fleet_handle, candidate_layout.key_nonce, blob->work.aad, aad_size,
        blob->work.ciphertext, blob->work.ciphertext + 32U, &blob->model_handle);
    mtfs_secure_zero(blob->work.ciphertext, blob->work.ciphertext_capacity);
    mtfs_secure_zero(blob->work.aad, blob->work.aad_capacity);
    if (crypto_result != MTFS_CRYPTO_OK ||
        blob->model_handle == MTFS_CRYPTO_INVALID_KEY_HANDLE)
    {
        if (crypto_result == MTFS_CRYPTO_OK)
            crypto_result = MTFS_CRYPTO_FAILED;
        return mtfs_open_fail(blob, mtfs_provider_error(crypto_result));
    }
    blob->info = candidate_info;
    blob->layout = candidate_layout;
    blob->state = MTFS_SEALED_BLOB_OPEN;
    *authenticated_info = candidate_info;
    return MTFS_OK;
}

static mtfs_error_t mtfs_load_fail(mtfs_sealed_blob_t *blob, void *destination,
                                   size_t payload_size, size_t *loaded_size,
                                   mtfs_error_t result)
{
    *loaded_size = 0U;
    mtfs_secure_zero(destination, payload_size);
    mtfs_secure_zero(blob->work.plaintext, blob->work.plaintext_capacity);
    mtfs_secure_zero(blob->work.ciphertext, blob->work.ciphertext_capacity);
    mtfs_secure_zero(blob->work.aad, blob->work.aad_capacity);
    mtfs_secure_zero(blob->work.manifest, blob->work.manifest_capacity);
    mtfs_close_handle(blob, &blob->model_handle);
    mtfs_close_handle(blob, &blob->fleet_handle);
    memset(&blob->info, 0, sizeof(blob->info));
    memset(&blob->layout, 0, sizeof(blob->layout));
    blob->state = MTFS_SEALED_BLOB_ERROR;
    return result;
}

mtfs_error_t mtfs_sealed_blob_load(mtfs_sealed_blob_t *blob,
    void *destination, size_t destination_size, size_t *loaded_size)
{
    uint64_t file_offset;
    uint64_t plain_offset = 0U;
    size_t payload_size;
    uint32_t index;
    if (blob == NULL || loaded_size == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    *loaded_size = 0U;
    if (blob->state != MTFS_SEALED_BLOB_OPEN)
        return MTFS_ERROR_INVALID_STATE;
    if (blob->info.payload_plain_length > SIZE_MAX || blob->info.required_ram > SIZE_MAX)
        return MTFS_ERROR_BUFFER_TOO_SMALL;
    payload_size = (size_t)blob->info.payload_plain_length;
    if (destination_size < (size_t)blob->info.required_ram ||
        destination_size < payload_size ||
        (destination == NULL && (payload_size != 0U || blob->info.required_ram != 0U)))
        return MTFS_ERROR_BUFFER_TOO_SMALL;
    if (mtfs_ranges_overlap(destination, destination_size, blob, sizeof(*blob)) ||
        mtfs_ranges_overlap(destination, destination_size, blob->work.manifest,
                            blob->work.manifest_capacity) ||
        mtfs_ranges_overlap(destination, destination_size, blob->work.aad,
                            blob->work.aad_capacity) ||
        mtfs_ranges_overlap(destination, destination_size, blob->work.ciphertext,
                            blob->work.ciphertext_capacity) ||
        mtfs_ranges_overlap(destination, destination_size, blob->work.plaintext,
                            blob->work.plaintext_capacity))
        return MTFS_ERROR_INVALID_ARGUMENT;
    blob->state = MTFS_SEALED_BLOB_LOADING;
    file_offset = blob->layout.payload_offset;
    for (index = 0U; index < blob->info.chunk_count; ++index)
    {
        uint64_t remaining = blob->info.payload_plain_length - plain_offset;
        size_t length = remaining < blob->info.chunk_plain_size ?
            (size_t)remaining : (size_t)blob->info.chunk_plain_size;
        size_t aad_size;
        uint8_t nonce[12];
        mtfs_error_t result;
        mtfs_crypto_status_t crypto_result;
        result = mtfs_read_exact(blob, file_offset, blob->work.ciphertext,
                                 length + MTFS_SEALED_TAG_SIZE);
        if (result != MTFS_OK)
            return mtfs_load_fail(blob, destination, payload_size, loaded_size, result);
        memcpy(nonce, blob->layout.payload_nonce_prefix, 8U);
        mtfs_put_u32(nonce + 8U, index);
        memcpy(blob->work.aad, mtfs_chunk_domain, sizeof(mtfs_chunk_domain));
        memcpy(blob->work.aad + sizeof(mtfs_chunk_domain), blob->work.manifest,
               blob->layout.manifest_size);
        mtfs_put_u32(blob->work.aad + sizeof(mtfs_chunk_domain) +
                     blob->layout.manifest_size, index);
        mtfs_put_u32(blob->work.aad + sizeof(mtfs_chunk_domain) +
                     blob->layout.manifest_size + 4U, (uint32_t)length);
        aad_size = sizeof(mtfs_chunk_domain) + blob->layout.manifest_size + 8U;
        crypto_result = blob->provider.decrypt_chunk(blob->provider.context,
            blob->model_handle, nonce, blob->work.aad, aad_size,
            blob->work.ciphertext, length, blob->work.ciphertext + length,
            blob->work.plaintext);
        mtfs_secure_zero(nonce, sizeof(nonce));
        mtfs_secure_zero(blob->work.aad, blob->work.aad_capacity);
        mtfs_secure_zero(blob->work.ciphertext, blob->work.ciphertext_capacity);
        if (crypto_result != MTFS_CRYPTO_OK)
            return mtfs_load_fail(blob, destination, payload_size, loaded_size,
                                  mtfs_provider_error(crypto_result));
        memcpy((uint8_t *)destination + (size_t)plain_offset,
               blob->work.plaintext, length);
        mtfs_secure_zero(blob->work.plaintext, blob->work.plaintext_capacity);
        plain_offset += length;
        file_offset += length + MTFS_SEALED_TAG_SIZE;
    }
    blob->state = MTFS_SEALED_BLOB_LOADED;
    *loaded_size = payload_size;
    return MTFS_OK;
}

mtfs_error_t mtfs_sealed_blob_close(mtfs_sealed_blob_t *blob)
{
    mtfs_error_t result = MTFS_OK;
    uint32_t api_version;
    uint32_t struct_size;
    if (blob == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    api_version = MTFS_SEALED_BLOB_API_VERSION;
    struct_size = (uint32_t)sizeof(*blob);
    if (blob->provider.close_key != NULL)
    {
        if (blob->model_handle != MTFS_CRYPTO_INVALID_KEY_HANDLE &&
            blob->provider.close_key(blob->provider.context, blob->model_handle) != MTFS_CRYPTO_OK)
            result = MTFS_ERROR_CRYPTO;
        blob->model_handle = MTFS_CRYPTO_INVALID_KEY_HANDLE;
        if (blob->fleet_handle != MTFS_CRYPTO_INVALID_KEY_HANDLE &&
            blob->provider.close_key(blob->provider.context, blob->fleet_handle) != MTFS_CRYPTO_OK)
            result = MTFS_ERROR_CRYPTO;
        blob->fleet_handle = MTFS_CRYPTO_INVALID_KEY_HANDLE;
    }
    mtfs_zero_work(&blob->work);
    mtfs_secure_zero(blob, sizeof(*blob));
    blob->api_version = api_version;
    blob->struct_size = struct_size;
    blob->state = MTFS_SEALED_BLOB_CLOSED;
    return result;
}

#else
typedef int mtfs_sealed_blob_disabled_translation_unit_t;
#endif /* MTFS_ENABLE_SEALED_MODEL */
