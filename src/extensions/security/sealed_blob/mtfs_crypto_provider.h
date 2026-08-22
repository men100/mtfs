#ifndef MTFS_CRYPTO_PROVIDER_H
#define MTFS_CRYPTO_PROVIDER_H

#include "mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_CRYPTO_PROVIDER_API_VERSION (1U)
#define MTFS_CRYPTO_INVALID_KEY_HANDLE (UINT32_C(0))

typedef uint32_t mtfs_crypto_key_handle_t;

typedef enum mtfs_crypto_status
{
    MTFS_CRYPTO_OK = 0,
    MTFS_CRYPTO_INVALID_ARGUMENT = 1,
    MTFS_CRYPTO_NOT_SUPPORTED = 2,
    MTFS_CRYPTO_AUTHENTICATION_FAILED = 3,
    MTFS_CRYPTO_KEY_NOT_FOUND = 4,
    MTFS_CRYPTO_RESOURCE_EXHAUSTED = 5,
    MTFS_CRYPTO_IO_FAILED = 6,
    MTFS_CRYPTO_FAILED = 7
} mtfs_crypto_status_t;

typedef struct mtfs_crypto_provider
{
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    mtfs_crypto_status_t (*open_fleet_key)(void *context, uint32_t key_id,
        uint32_t key_version, mtfs_crypto_key_handle_t *fleet_handle);
    mtfs_crypto_status_t (*open_model_key)(void *context,
        mtfs_crypto_key_handle_t fleet_handle, const uint8_t nonce[12],
        const uint8_t *aad, size_t aad_size, const uint8_t ciphertext[32],
        const uint8_t tag[16], mtfs_crypto_key_handle_t *model_handle);
    mtfs_crypto_status_t (*decrypt_chunk)(void *context,
        mtfs_crypto_key_handle_t model_handle, const uint8_t nonce[12],
        const uint8_t *aad, size_t aad_size, const uint8_t *ciphertext,
        size_t ciphertext_size, const uint8_t tag[16], uint8_t *plaintext);
    mtfs_crypto_status_t (*close_key)(void *context,
        mtfs_crypto_key_handle_t handle);
} mtfs_crypto_provider_t;

#ifdef __cplusplus
}
#endif
#endif /* MTFS_ENABLE_SEALED_MODEL */

#endif /* MTFS_CRYPTO_PROVIDER_H */
