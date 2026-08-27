#include "../../../mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL

#include "mtfs_ra8p1_rsip_provider.h"

#include <string.h>

#include "r_rsip_key_injection.h"
#include "rm_psa_crypto.h"
#include "../../../extensions/security/sealed_blob/mtfs_secure_zero.h"

#define FLEET_HANDLE_TAG       (UINT32_C(0xf17e0000))
#define MODEL_HANDLE_TAG       (UINT32_C(0x4d4f0000))
#define HANDLE_TAG_MASK        (UINT32_C(0xffff0000))
#define HANDLE_GENERATION_MASK (UINT32_C(0x0000ffff))

_Static_assert(MTFS_SEALED_KEY_AAD_SIZE == 4268U,
    "RA8P1 envelope AAD contract changed");
_Static_assert(MTFS_SEALED_CHUNK_AAD_SIZE == 4278U,
    "RA8P1 chunk AAD contract changed");

static int valid_context(const mtfs_ra8p1_rsip_provider_context_t *context)
{
    return context != NULL &&
        context->api_version == MTFS_RA8P1_RSIP_PROVIDER_API_VERSION &&
        context->struct_size >= sizeof(*context) &&
        context->initialized != 0U && context->lock != NULL &&
        context->unlock != NULL;
}

static mtfs_crypto_status_t lock_provider(
    mtfs_ra8p1_rsip_provider_context_t *context)
{
    if (!valid_context(context))
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    return context->lock(context->lock_context) == 0 ? MTFS_CRYPTO_OK :
        MTFS_CRYPTO_RESOURCE_EXHAUSTED;
}

static mtfs_crypto_key_handle_t next_handle(
    mtfs_ra8p1_rsip_provider_context_t *context, uint32_t tag)
{
    context->handle_generation =
        (context->handle_generation + 1U) & HANDLE_GENERATION_MASK;
    if (context->handle_generation == 0U)
        context->handle_generation = 1U;
    return tag | context->handle_generation;
}

static mtfs_crypto_status_t map_store(mtfs_ra8p1_key_store_status_t status)
{
    if (status == MTFS_RA8P1_KEY_STORE_NOT_FOUND)
        return MTFS_CRYPTO_KEY_NOT_FOUND;
    if (status == MTFS_RA8P1_KEY_STORE_INVALID_ARGUMENT)
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    return status == MTFS_RA8P1_KEY_STORE_OK ? MTFS_CRYPTO_OK :
        MTFS_CRYPTO_IO_FAILED;
}

static mtfs_crypto_status_t map_psa(psa_status_t status, int decrypt)
{
    if (status == PSA_SUCCESS)
        return MTFS_CRYPTO_OK;
    if (status == PSA_ERROR_INVALID_ARGUMENT)
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    if (status == PSA_ERROR_NOT_SUPPORTED)
        return MTFS_CRYPTO_NOT_SUPPORTED;
    if (status == PSA_ERROR_INSUFFICIENT_MEMORY ||
        status == PSA_ERROR_INSUFFICIENT_STORAGE ||
        status == PSA_ERROR_BUFFER_TOO_SMALL)
        return MTFS_CRYPTO_RESOURCE_EXHAUSTED;
    if (status == PSA_ERROR_DOES_NOT_EXIST)
        return MTFS_CRYPTO_KEY_NOT_FOUND;
    /* FSP 6.5 maps a confirmed RSIP GCM authentication mismatch to -147
     * (PSA_ERROR_HARDWARE_FAILURE).  Treat -147 as authentication rejection
     * only at an AEAD-decrypt result site and preserve the raw status. */
    if (decrypt && (status == PSA_ERROR_INVALID_SIGNATURE ||
        status == PSA_ERROR_HARDWARE_FAILURE))
        return MTFS_CRYPTO_AUTHENTICATION_FAILED;
    return MTFS_CRYPTO_FAILED;
}

static psa_status_t import_wrapped(const rsip_aes_wrapped_key_t *wrapped,
    psa_key_handle_t *handle)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_status_t status;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES_WRAPPED);
    psa_set_key_bits(&attributes, 256U);
    psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_VOLATILE);
    status = psa_import_key(&attributes, (const uint8_t *)wrapped->value,
        MTFS_RA8P1_AES256_WRAPPED_BYTES, handle);
    psa_reset_key_attributes(&attributes);
    return status;
}

static mtfs_crypto_status_t open_fleet_key(void *opaque, uint32_t key_id,
    uint32_t key_version, mtfs_crypto_key_handle_t *fleet_handle)
{
    mtfs_ra8p1_rsip_provider_context_t *context = opaque;
    rsip_aes_wrapped_key_t wrapped;
    mtfs_ra8p1_key_metadata_t metadata;
    mtfs_crypto_status_t result;
    psa_status_t status;
    if (fleet_handle == NULL || key_id == 0U || key_version == 0U)
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    *fleet_handle = MTFS_CRYPTO_INVALID_KEY_HANDLE;
    memset(&wrapped, 0, sizeof(wrapped));
    memset(&metadata, 0, sizeof(metadata));
    result = lock_provider(context);
    if (result != MTFS_CRYPTO_OK)
        return result;
    if (context->fleet_open != 0U || context->model_open != 0U) {
        result = MTFS_CRYPTO_RESOURCE_EXHAUSTED;
        goto cleanup;
    }
    context->store_diagnostics.last_status =
        mtfs_ra8p1_ospi_key_store_load(&wrapped, &metadata,
            &context->store_diagnostics);
    result = map_store(context->store_diagnostics.last_status);
    if (result != MTFS_CRYPTO_OK)
        goto cleanup;
    if (metadata.key_id != key_id || metadata.key_version != key_version) {
        result = MTFS_CRYPTO_KEY_NOT_FOUND;
        goto cleanup;
    }
    status = import_wrapped(&wrapped, &context->fleet_psa_handle);
    context->last_psa_status = (int32_t)status;
    result = map_psa(status, 0);
    if (result != MTFS_CRYPTO_OK) {
        context->fleet_psa_handle = 0U;
        goto cleanup;
    }
    context->fleet_metadata = metadata;
    context->fleet_handle = next_handle(context, FLEET_HANDLE_TAG);
    context->fleet_open = 1U;
    *fleet_handle = context->fleet_handle;

cleanup:
    mtfs_secure_zero(&wrapped, sizeof(wrapped));
    mtfs_secure_zero(&metadata, sizeof(metadata));
    context->unlock(context->lock_context);
    return result;
}

static mtfs_crypto_status_t open_model_key(void *opaque,
    mtfs_crypto_key_handle_t fleet_handle, const uint8_t nonce[12],
    const uint8_t *aad, size_t aad_size, const uint8_t ciphertext[32],
    const uint8_t tag[16], mtfs_crypto_key_handle_t *model_handle)
{
    mtfs_ra8p1_rsip_provider_context_t *context = opaque;
    mtfs_crypto_status_t result;
    psa_status_t status;
    size_t output_size = 0U;
    if (aad_size > MTFS_SEALED_KEY_AAD_SIZE)
        return MTFS_CRYPTO_RESOURCE_EXHAUSTED;
    if (model_handle == NULL || nonce == NULL || ciphertext == NULL ||
        tag == NULL || (aad == NULL && aad_size != 0U))
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    *model_handle = MTFS_CRYPTO_INVALID_KEY_HANDLE;
    result = lock_provider(context);
    if (result != MTFS_CRYPTO_OK)
        return result;
    if (context->fleet_open == 0U || fleet_handle != context->fleet_handle ||
        (fleet_handle & HANDLE_TAG_MASK) != FLEET_HANDLE_TAG ||
        context->model_open != 0U) {
        result = MTFS_CRYPTO_INVALID_ARGUMENT;
        goto cleanup;
    }
    memcpy(context->combined, ciphertext, 32U);
    memcpy(context->combined + 32U, tag, 16U);
    status = psa_aead_decrypt(context->fleet_psa_handle, PSA_ALG_GCM,
        nonce, 12U, aad, aad_size, context->combined, 48U,
        context->model_key_work.psa_output,
        sizeof(context->model_key_work.psa_output), &output_size);
    context->last_psa_status = (int32_t)status;
    result = map_psa(status, 1);
    if (result != MTFS_CRYPTO_OK || output_size != 32U) {
        if (result == MTFS_CRYPTO_OK)
            result = MTFS_CRYPTO_FAILED;
        goto cleanup;
    }
    context->last_fsp_status = (int32_t)R_RSIP_AES256_InitialKeyWrap(
        RSIP_KEY_INJECTION_TYPE_PLAIN, NULL, NULL,
        context->model_key_work.fields.raw_key, &context->wrapped_model_key);
    if (context->last_fsp_status != (int32_t)FSP_SUCCESS) {
        result = MTFS_CRYPTO_FAILED;
        goto cleanup;
    }
    status = import_wrapped(&context->wrapped_model_key,
        &context->model_psa_handle);
    context->last_psa_status = (int32_t)status;
    result = map_psa(status, 0);
    if (result != MTFS_CRYPTO_OK) {
        context->model_psa_handle = 0U;
        goto cleanup;
    }
    context->model_handle = next_handle(context, MODEL_HANDLE_TAG);
    context->model_open = 1U;
    *model_handle = context->model_handle;

cleanup:
    mtfs_secure_zero(&context->model_key_work,
        sizeof(context->model_key_work));
    mtfs_secure_zero(&context->wrapped_model_key,
        sizeof(context->wrapped_model_key));
    mtfs_secure_zero(context->combined, 48U);
    if (result != MTFS_CRYPTO_OK && context->model_psa_handle != 0U) {
        context->last_psa_status =
            (int32_t)psa_destroy_key(context->model_psa_handle);
        context->model_psa_handle = 0U;
    }
    context->unlock(context->lock_context);
    return result;
}

static mtfs_crypto_status_t decrypt_chunk(void *opaque,
    mtfs_crypto_key_handle_t model_handle, const uint8_t nonce[12],
    const uint8_t *aad, size_t aad_size, const uint8_t *ciphertext,
    size_t ciphertext_size, const uint8_t tag[16], uint8_t *plaintext)
{
    mtfs_ra8p1_rsip_provider_context_t *context = opaque;
    mtfs_crypto_status_t result;
    psa_status_t status;
    size_t output_size = 0U;
    size_t combined_size;
    /* Preflight length rejection must not acquire RSIP or touch output. */
    if (ciphertext_size > MTFS_SEALED_MAX_CHUNK_SIZE ||
        aad_size > MTFS_SEALED_CHUNK_AAD_SIZE)
        return MTFS_CRYPTO_RESOURCE_EXHAUSTED;
    if (nonce == NULL || tag == NULL || plaintext == NULL ||
        (aad == NULL && aad_size != 0U) ||
        (ciphertext == NULL && ciphertext_size != 0U)) {
        if (plaintext != NULL)
            mtfs_secure_zero(plaintext, ciphertext_size);
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    }
    combined_size = ciphertext_size + MTFS_SEALED_TAG_SIZE;
    result = lock_provider(context);
    if (result != MTFS_CRYPTO_OK) {
        mtfs_secure_zero(plaintext, ciphertext_size);
        return result;
    }
    if (context->model_open == 0U || model_handle != context->model_handle ||
        (model_handle & HANDLE_TAG_MASK) != MODEL_HANDLE_TAG) {
        result = MTFS_CRYPTO_INVALID_ARGUMENT;
        goto failed;
    }
    if (ciphertext_size != 0U)
        memcpy(context->combined, ciphertext, ciphertext_size);
    memcpy(context->combined + ciphertext_size, tag, MTFS_SEALED_TAG_SIZE);
    status = psa_aead_decrypt(context->model_psa_handle, PSA_ALG_GCM,
        nonce, 12U, aad, aad_size, context->combined, combined_size,
        context->authenticated, sizeof(context->authenticated), &output_size);
    context->last_psa_status = (int32_t)status;
    result = map_psa(status, 1);
    if (result != MTFS_CRYPTO_OK || output_size != ciphertext_size) {
        if (result == MTFS_CRYPTO_OK)
            result = MTFS_CRYPTO_FAILED;
        goto failed;
    }
    if (ciphertext_size != 0U)
        memcpy(plaintext, context->authenticated, ciphertext_size);
    goto cleanup;

failed:
    mtfs_secure_zero(plaintext, ciphertext_size);
cleanup:
    mtfs_secure_zero(context->combined, combined_size);
    mtfs_secure_zero(context->authenticated,
        ciphertext_size + MTFS_RA8P1_RSIP_PSA_TAIL_BYTES);
    context->unlock(context->lock_context);
    return result;
}

static mtfs_crypto_status_t close_key(void *opaque,
    mtfs_crypto_key_handle_t handle)
{
    mtfs_ra8p1_rsip_provider_context_t *context = opaque;
    mtfs_crypto_status_t result = lock_provider(context);
    psa_status_t status;
    if (result != MTFS_CRYPTO_OK)
        return result;
    if (context->model_open != 0U && handle == context->model_handle &&
        (handle & HANDLE_TAG_MASK) == MODEL_HANDLE_TAG) {
        status = psa_destroy_key(context->model_psa_handle);
        context->last_psa_status = (int32_t)status;
        result = map_psa(status, 0);
        if (result == MTFS_CRYPTO_OK) {
            context->model_psa_handle = 0U;
            context->model_handle = MTFS_CRYPTO_INVALID_KEY_HANDLE;
            context->model_open = 0U;
        }
        mtfs_secure_zero(&context->model_key_work,
            sizeof(context->model_key_work));
        mtfs_secure_zero(&context->wrapped_model_key,
            sizeof(context->wrapped_model_key));
        mtfs_secure_zero(context->combined, sizeof(context->combined));
        mtfs_secure_zero(context->authenticated,
            sizeof(context->authenticated));
    } else if (context->fleet_open != 0U && handle == context->fleet_handle &&
        (handle & HANDLE_TAG_MASK) == FLEET_HANDLE_TAG &&
        context->model_open == 0U) {
        status = psa_destroy_key(context->fleet_psa_handle);
        context->last_psa_status = (int32_t)status;
        result = map_psa(status, 0);
        if (result == MTFS_CRYPTO_OK) {
            context->fleet_psa_handle = 0U;
            context->fleet_handle = MTFS_CRYPTO_INVALID_KEY_HANDLE;
            context->fleet_open = 0U;
            mtfs_secure_zero(&context->fleet_metadata,
                sizeof(context->fleet_metadata));
        }
    } else {
        result = MTFS_CRYPTO_INVALID_ARGUMENT;
    }
    context->unlock(context->lock_context);
    return result;
}

mtfs_crypto_status_t mtfs_ra8p1_rsip_provider_init(
    mtfs_ra8p1_rsip_provider_context_t *context,
    mtfs_ra8p1_rsip_lock_fn lock, mtfs_ra8p1_rsip_unlock_fn unlock,
    void *lock_context, mtfs_crypto_provider_t *provider)
{
    psa_status_t status;
    if (context == NULL || lock == NULL || unlock == NULL || provider == NULL)
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    memset(context, 0, sizeof(*context));
    memset(provider, 0, sizeof(*provider));
    context->api_version = MTFS_RA8P1_RSIP_PROVIDER_API_VERSION;
    context->struct_size = (uint32_t)sizeof(*context);
    context->lock = lock;
    context->unlock = unlock;
    context->lock_context = lock_context;
    context->initialized = 1U;
    if (lock_provider(context) != MTFS_CRYPTO_OK) {
        mtfs_secure_zero(context, sizeof(*context));
        return MTFS_CRYPTO_RESOURCE_EXHAUSTED;
    }
    status = psa_crypto_init();
    context->last_psa_status = (int32_t)status;
    context->unlock(context->lock_context);
    if (status != PSA_SUCCESS) {
        mtfs_secure_zero(context, sizeof(*context));
        return map_psa(status, 0);
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

mtfs_crypto_status_t mtfs_ra8p1_rsip_provider_deinit(
    mtfs_ra8p1_rsip_provider_context_t *context)
{
    mtfs_crypto_status_t result = MTFS_CRYPTO_OK;
    psa_status_t status;
    if (!valid_context(context))
        return MTFS_CRYPTO_INVALID_ARGUMENT;
    if (lock_provider(context) != MTFS_CRYPTO_OK)
        return MTFS_CRYPTO_RESOURCE_EXHAUSTED;
    if (context->model_psa_handle != 0U) {
        status = psa_destroy_key(context->model_psa_handle);
        context->last_psa_status = (int32_t)status;
        if (status != PSA_SUCCESS) {
            result = map_psa(status, 0);
            mtfs_secure_zero(&context->model_key_work,
                sizeof(context->model_key_work));
            mtfs_secure_zero(&context->wrapped_model_key,
                sizeof(context->wrapped_model_key));
            mtfs_secure_zero(context->combined, sizeof(context->combined));
            mtfs_secure_zero(context->authenticated,
                sizeof(context->authenticated));
            context->unlock(context->lock_context);
            return result;
        }
        context->model_psa_handle = 0U;
        context->model_handle = MTFS_CRYPTO_INVALID_KEY_HANDLE;
        context->model_open = 0U;
    }
    if (context->fleet_psa_handle != 0U) {
        status = psa_destroy_key(context->fleet_psa_handle);
        context->last_psa_status = (int32_t)status;
        if (status != PSA_SUCCESS) {
            result = map_psa(status, 0);
            context->unlock(context->lock_context);
            return result;
        }
        context->fleet_psa_handle = 0U;
        context->fleet_handle = MTFS_CRYPTO_INVALID_KEY_HANDLE;
        context->fleet_open = 0U;
    }
    context->unlock(context->lock_context);
    mtfs_secure_zero(context, sizeof(*context));
    return result;
}

#else
typedef int mtfs_ra8p1_rsip_provider_disabled_translation_unit_t;
#endif
