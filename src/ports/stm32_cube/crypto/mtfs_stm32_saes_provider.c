#include "../../../mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL

#include "mtfs_stm32_saes_provider.h"

#include <string.h>

#include "../../../extensions/security/sealed_blob/mtfs_sealed_format.h"

#define FLEET_HANDLE_TAG (UINT32_C(0xf17e0000))
#define MODEL_HANDLE_TAG (UINT32_C(0x4d4f0000))
#define HANDLE_TAG_MASK  (UINT32_C(0xffff0000))
#define HANDLE_GENERATION_MASK (UINT32_C(0x0000ffff))

typedef char wrapped_key_size_match[
    sizeof(mtfs_stm32_nor_wrapped_key_t) ==
        sizeof(mtfs_stm32_saes_wrapped_key_t) ? 1 : -1];
typedef char sealed_aad_fits_saes_primitive[
    MTFS_STM32_SAES_MAX_AAD_BYTES >= MTFS_SEALED_CHUNK_AAD_SIZE ? 1 : -1];

static int valid_context(const mtfs_stm32_saes_provider_context_t *context)
{
    return context != NULL &&
        context->api_version == MTFS_STM32_SAES_PROVIDER_API_VERSION &&
        context->struct_size >= sizeof(*context) &&
        context->initialized != 0U && context->lock != NULL &&
        context->unlock != NULL;
}

static mtfs_crypto_status_t lock_provider(
    mtfs_stm32_saes_provider_context_t *context)
{
    if (!valid_context(context))
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    return context->lock(context->lock_context) == 0 ? MTFS_CRYPTO_OK :
        MTFS_CRYPTO_RESOURCE_EXHAUSTED;
}

static mtfs_crypto_key_handle_t next_handle(
    mtfs_stm32_saes_provider_context_t *context, uint32_t tag)
{
    context->handle_generation =
        (context->handle_generation + 1U) & HANDLE_GENERATION_MASK;
    if (context->handle_generation == 0U)
        context->handle_generation = 1U;
    return tag | context->handle_generation;
}

static mtfs_crypto_status_t map_saes(mtfs_stm32_saes_status_t status)
{
    switch (status) {
    case MTFS_STM32_SAES_OK:
        return MTFS_CRYPTO_OK;
    case MTFS_STM32_SAES_INVALID_ARGUMENT:
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    case MTFS_STM32_SAES_TOO_LARGE:
        return MTFS_CRYPTO_RESOURCE_EXHAUSTED;
    case MTFS_STM32_SAES_AUTH_FAILED:
        return MTFS_CRYPTO_AUTHENTICATION_FAILED;
    case MTFS_STM32_SAES_HAL_ERROR:
    default:
        return MTFS_CRYPTO_FAILED;
    }
}

static mtfs_crypto_status_t open_fleet_key(void *opaque, uint32_t key_id,
    uint32_t key_version, mtfs_crypto_key_handle_t *fleet_handle)
{
    mtfs_stm32_saes_provider_context_t *context =
        (mtfs_stm32_saes_provider_context_t *)opaque;
    mtfs_stm32_nor_wrapped_key_t stored;
    mtfs_stm32_nor_key_metadata_t metadata;
    mtfs_stm32_nor_key_store_diagnostics_t diagnostics;
    mtfs_crypto_status_t result;
    if (fleet_handle == NULL || key_id == 0U || key_version == 0U)
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    *fleet_handle = MTFS_CRYPTO_INVALID_KEY_HANDLE;
    memset(&stored, 0, sizeof(stored));
    memset(&metadata, 0, sizeof(metadata));
    memset(&diagnostics, 0, sizeof(diagnostics));
    result = lock_provider(context);
    if (result != MTFS_CRYPTO_OK)
        return result;
    if (context->fleet_open != 0U || context->model_open != 0U) {
        result = MTFS_CRYPTO_RESOURCE_EXHAUSTED;
        goto cleanup;
    }
    context->last_store_status = mtfs_stm32_nor_key_store_load(&context->nor,
        &stored, &metadata, &diagnostics);
    if (context->last_store_status == MTFS_STM32_NOR_KEY_STORE_NOT_FOUND) {
        result = MTFS_CRYPTO_KEY_NOT_FOUND;
        goto cleanup;
    }
    if (context->last_store_status != MTFS_STM32_NOR_KEY_STORE_OK) {
        result = MTFS_CRYPTO_IO_FAILED;
        goto cleanup;
    }
    /* The STM32 MTFK v1 schema and fixed 32-byte blob identify an SAES/DHUK
     * fleet key record.  The key store has validated format/header/blob
     * lengths, CRC, commit marker and generation; MTFK v1 has no explicit
     * provider or key-type fields.  Match its key selectors to the package. */
    if (metadata.key_id != key_id || metadata.key_version != key_version) {
        result = MTFS_CRYPTO_KEY_NOT_FOUND;
        goto cleanup;
    }
    memcpy(&context->fleet_key, &stored, sizeof(context->fleet_key));
    context->fleet_metadata = metadata;
    context->fleet_handle = next_handle(context, FLEET_HANDLE_TAG);
    context->fleet_open = 1U;
    *fleet_handle = context->fleet_handle;
    result = MTFS_CRYPTO_OK;

cleanup:
    mtfs_stm32_saes_zeroize(&stored, sizeof(stored));
    mtfs_stm32_saes_zeroize(&metadata, sizeof(metadata));
    mtfs_stm32_saes_zeroize(&diagnostics, sizeof(diagnostics));
    context->unlock(context->lock_context);
    return result;
}

static mtfs_crypto_status_t open_model_key(void *opaque,
    mtfs_crypto_key_handle_t fleet_handle, const uint8_t nonce[12],
    const uint8_t *aad, size_t aad_size, const uint8_t ciphertext[32],
    const uint8_t tag[16], mtfs_crypto_key_handle_t *model_handle)
{
    mtfs_stm32_saes_provider_context_t *context =
        (mtfs_stm32_saes_provider_context_t *)opaque;
    mtfs_crypto_status_t result;
    if (model_handle == NULL || nonce == NULL || ciphertext == NULL ||
        tag == NULL || (aad == NULL && aad_size != 0U))
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    *model_handle = MTFS_CRYPTO_INVALID_KEY_HANDLE;
    result = lock_provider(context);
    if (result != MTFS_CRYPTO_OK)
        return result;
    if (context->fleet_open == 0U || fleet_handle != context->fleet_handle ||
        context->model_open != 0U) {
        result = MTFS_CRYPTO_INVALID_ARGUMENT;
        goto cleanup;
    }
    context->last_saes_status = mtfs_stm32_saes_decrypt_wrapped(&context->saes,
        &context->fleet_key, nonce, aad, aad_size, ciphertext, 32U, tag,
        context->raw_model_key);
    result = map_saes(context->last_saes_status);
    if (result != MTFS_CRYPTO_OK)
        goto cleanup;
    context->last_saes_status = mtfs_stm32_saes_wrap_key(&context->saes,
        context->raw_model_key, &context->model_key);
    result = map_saes(context->last_saes_status);
    if (result != MTFS_CRYPTO_OK) {
        mtfs_stm32_saes_zeroize(&context->model_key,
            sizeof(context->model_key));
        goto cleanup;
    }
    context->model_handle = next_handle(context, MODEL_HANDLE_TAG);
    context->model_open = 1U;
    *model_handle = context->model_handle;

cleanup:
    mtfs_stm32_saes_zeroize(context->raw_model_key,
        sizeof(context->raw_model_key));
    context->unlock(context->lock_context);
    return result;
}

static mtfs_crypto_status_t decrypt_chunk(void *opaque,
    mtfs_crypto_key_handle_t model_handle, const uint8_t nonce[12],
    const uint8_t *aad, size_t aad_size, const uint8_t *ciphertext,
    size_t ciphertext_size, const uint8_t tag[16], uint8_t *plaintext)
{
    mtfs_stm32_saes_provider_context_t *context =
        (mtfs_stm32_saes_provider_context_t *)opaque;
    mtfs_crypto_status_t result;
    /* Reject an unrepresentable output length before inspecting pointers,
     * acquiring the SAES lock, starting hardware, or touching plaintext. */
    if (ciphertext_size > MTFS_STM32_SAES_MAX_DATA_BYTES)
        return MTFS_CRYPTO_RESOURCE_EXHAUSTED;
    if (nonce == NULL || tag == NULL || plaintext == NULL ||
        (aad == NULL && aad_size != 0U) ||
        (ciphertext == NULL && ciphertext_size != 0U)) {
        if (plaintext != NULL)
            mtfs_stm32_saes_zeroize(plaintext, ciphertext_size);
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    }
    if (aad_size > MTFS_SEALED_CHUNK_AAD_SIZE) {
        mtfs_stm32_saes_zeroize(plaintext, ciphertext_size);
        return MTFS_CRYPTO_RESOURCE_EXHAUSTED;
    }
    result = lock_provider(context);
    if (result != MTFS_CRYPTO_OK) {
        mtfs_stm32_saes_zeroize(plaintext, ciphertext_size);
        return result;
    }
    if (context->model_open == 0U || model_handle != context->model_handle ||
        (model_handle & HANDLE_TAG_MASK) != MODEL_HANDLE_TAG) {
        mtfs_stm32_saes_zeroize(plaintext, ciphertext_size);
        result = MTFS_CRYPTO_INVALID_ARGUMENT;
        goto cleanup;
    }
    context->last_saes_status = mtfs_stm32_saes_decrypt_wrapped(&context->saes,
        &context->model_key, nonce, aad, aad_size, ciphertext,
        ciphertext_size, tag, plaintext);
    result = map_saes(context->last_saes_status);
    if (result != MTFS_CRYPTO_OK)
        mtfs_stm32_saes_zeroize(plaintext, ciphertext_size);

cleanup:
    context->unlock(context->lock_context);
    return result;
}

static mtfs_crypto_status_t close_key(void *opaque,
    mtfs_crypto_key_handle_t handle)
{
    mtfs_stm32_saes_provider_context_t *context =
        (mtfs_stm32_saes_provider_context_t *)opaque;
    mtfs_crypto_status_t result = lock_provider(context);
    if (result != MTFS_CRYPTO_OK)
        return result;
    if (context->model_open != 0U && handle == context->model_handle &&
        (handle & HANDLE_TAG_MASK) == MODEL_HANDLE_TAG) {
        mtfs_stm32_saes_zeroize(&context->model_key,
            sizeof(context->model_key));
        mtfs_stm32_saes_zeroize(context->raw_model_key,
            sizeof(context->raw_model_key));
        context->model_handle = MTFS_CRYPTO_INVALID_KEY_HANDLE;
        context->model_open = 0U;
        result = MTFS_CRYPTO_OK;
    } else if (context->fleet_open != 0U && handle == context->fleet_handle &&
        (handle & HANDLE_TAG_MASK) == FLEET_HANDLE_TAG &&
        context->model_open == 0U) {
        mtfs_stm32_saes_zeroize(&context->fleet_key,
            sizeof(context->fleet_key));
        mtfs_stm32_saes_zeroize(&context->fleet_metadata,
            sizeof(context->fleet_metadata));
        context->fleet_handle = MTFS_CRYPTO_INVALID_KEY_HANDLE;
        context->fleet_open = 0U;
        result = MTFS_CRYPTO_OK;
    } else {
        result = MTFS_CRYPTO_INVALID_ARGUMENT;
    }
    context->unlock(context->lock_context);
    return result;
}

mtfs_crypto_status_t mtfs_stm32_saes_provider_init(
    mtfs_stm32_saes_provider_context_t *context,
    const mtfs_stm32_nor_io_t *nor, mtfs_stm32_saes_provider_lock_fn lock,
    mtfs_stm32_saes_provider_unlock_fn unlock, void *lock_context,
    mtfs_crypto_provider_t *provider)
{
    mtfs_stm32_saes_status_t saes_status;
    if (context == NULL || nor == NULL || nor->read == NULL ||
        nor->erase_sector == NULL || nor->program == NULL || lock == NULL ||
        unlock == NULL || provider == NULL)
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    memset(context, 0, sizeof(*context));
    memset(provider, 0, sizeof(*provider));
    context->api_version = MTFS_STM32_SAES_PROVIDER_API_VERSION;
    context->struct_size = (uint32_t)sizeof(*context);
    context->nor = *nor;
    context->lock = lock;
    context->unlock = unlock;
    context->lock_context = lock_context;
    context->initialized = 1U;
    if (lock(lock_context) != 0) {
        context->initialized = 0U;
        return MTFS_CRYPTO_RESOURCE_EXHAUSTED;
    }
    saes_status = mtfs_stm32_saes_init(&context->saes);
    context->last_saes_status = saes_status;
    unlock(lock_context);
    if (saes_status != MTFS_STM32_SAES_OK) {
        context->initialized = 0U;
        return map_saes(saes_status);
    }
    provider->api_version = MTFS_CRYPTO_PROVIDER_API_VERSION;
    provider->struct_size = (uint32_t)sizeof(*provider);
    provider->context = context;
    provider->open_fleet_key = open_fleet_key;
    provider->open_model_key = open_model_key;
    provider->decrypt_chunk = decrypt_chunk;
    provider->close_key = close_key;
    return MTFS_CRYPTO_OK;
}

mtfs_crypto_status_t mtfs_stm32_saes_provider_deinit(
    mtfs_stm32_saes_provider_context_t *context)
{
    uint32_t api_version = MTFS_STM32_SAES_PROVIDER_API_VERSION;
    uint32_t struct_size = (uint32_t)sizeof(*context);
    mtfs_stm32_saes_provider_unlock_fn unlock;
    void *lock_context;
    if (!valid_context(context))
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    if (context->lock(context->lock_context) != 0)
        return MTFS_CRYPTO_RESOURCE_EXHAUSTED;
    unlock = context->unlock;
    lock_context = context->lock_context;
    mtfs_stm32_saes_zeroize(&context->fleet_key, sizeof(context->fleet_key));
    mtfs_stm32_saes_zeroize(&context->model_key, sizeof(context->model_key));
    mtfs_stm32_saes_zeroize(context->raw_model_key,
        sizeof(context->raw_model_key));
    mtfs_stm32_saes_zeroize(context, sizeof(*context));
    context->api_version = api_version;
    context->struct_size = struct_size;
    unlock(lock_context);
    return MTFS_CRYPTO_OK;
}

#else
typedef int mtfs_stm32_saes_provider_disabled_translation_unit_t;
#endif /* MTFS_ENABLE_SEALED_MODEL */
