/** @file mtfs_crypto_provider.h
 * @brief Opaque-key authenticated decryption provider. / opaque keyを使用する認証付き復号provider。
 * @ingroup mtfs_sealed */
#ifndef MTFS_CRYPTO_PROVIDER_H
#define MTFS_CRYPTO_PROVIDER_H

/** @addtogroup mtfs_sealed
 * @{ */

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

/*
 * Providers expose opaque handles only; plaintext fleet/model keys never cross this boundary.
 * providerは内部のkey内容を直接扱えないopaque handleのみを公開し、plaintextのfleet/model keyがproviderの外部に渡されることはない。
 */

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

/** @brief Hardware-backed key and AES-256-GCM operation table. / hardware-backed keyとAES-256-GCM操作を提供するoperation table。
 * @details Handles are provider-owned and valid until close_key. open_model_key and decrypt_chunk authenticate before exposing usable key/plaintext results. Plaintext fleet/model keys never cross this boundary.
 * / key handleはproviderが所有し、close_keyまで有効。open_model_keyとdecrypt_chunkは認証に成功した場合にのみ、利用可能なkey handleまたはplaintextを返す。plaintextのfleet/model keyがproviderの外部に渡されることはない。
 * @warning Callbacks execute in caller task context and may require external serialization defined by the target provider. / callbackは呼び出し側のtask contextで実行される。target providerの要件によっては、呼び出し側でproviderへのアクセスを直列化する必要がある。 */
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

/** @} */

#endif /* MTFS_CRYPTO_PROVIDER_H */
