#ifndef MTFS_STM32_SAES_H
#define MTFS_STM32_SAES_H

#include <stddef.h>
#include <stdint.h>

#include "stm32n6xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_STM32_SAES_AES256_KEY_BYTES     (32U)
#define MTFS_STM32_SAES_WRAPPED_KEY_BYTES    (32U)
#define MTFS_STM32_SAES_GCM_NONCE_BYTES      (12U)
#define MTFS_STM32_SAES_GCM_TAG_BYTES        (16U)
#define MTFS_STM32_SAES_MAX_DATA_BYTES       (64U * 1024U)
#define MTFS_STM32_SAES_MAX_AAD_BYTES        (4U * 1024U)

typedef union mtfs_stm32_saes_wrapped_key
{
    uint32_t words[MTFS_STM32_SAES_WRAPPED_KEY_BYTES / sizeof(uint32_t)];
    uint8_t bytes[MTFS_STM32_SAES_WRAPPED_KEY_BYTES];
} mtfs_stm32_saes_wrapped_key_t;

typedef enum mtfs_stm32_saes_status
{
    MTFS_STM32_SAES_OK = 0,
    MTFS_STM32_SAES_INVALID_ARGUMENT = -1,
    MTFS_STM32_SAES_TOO_LARGE = -2,
    MTFS_STM32_SAES_HAL_ERROR = -3,
    MTFS_STM32_SAES_AUTH_FAILED = -4
} mtfs_stm32_saes_status_t;

typedef struct mtfs_stm32_saes_context
{
    CRYP_HandleTypeDef cryp;
    RNG_HandleTypeDef rng;
    uint32_t last_hal_status;
    uint32_t last_hal_error;
    uint8_t rng_ready;
} mtfs_stm32_saes_context_t;

mtfs_stm32_saes_status_t mtfs_stm32_saes_init(
    mtfs_stm32_saes_context_t *context);

mtfs_stm32_saes_status_t mtfs_stm32_saes_wrap_key(
    mtfs_stm32_saes_context_t *context,
    const uint8_t raw_key[MTFS_STM32_SAES_AES256_KEY_BYTES],
    mtfs_stm32_saes_wrapped_key_t *wrapped_key);

mtfs_stm32_saes_status_t mtfs_stm32_saes_encrypt_wrapped(
    mtfs_stm32_saes_context_t *context,
    const mtfs_stm32_saes_wrapped_key_t *wrapped_key,
    const uint8_t nonce[MTFS_STM32_SAES_GCM_NONCE_BYTES],
    const uint8_t *aad,
    size_t aad_bytes,
    const uint8_t *plaintext,
    size_t plaintext_bytes,
    uint8_t *ciphertext,
    uint8_t tag[MTFS_STM32_SAES_GCM_TAG_BYTES]);

mtfs_stm32_saes_status_t mtfs_stm32_saes_decrypt_wrapped(
    mtfs_stm32_saes_context_t *context,
    const mtfs_stm32_saes_wrapped_key_t *wrapped_key,
    const uint8_t nonce[MTFS_STM32_SAES_GCM_NONCE_BYTES],
    const uint8_t *aad,
    size_t aad_bytes,
    const uint8_t *ciphertext,
    size_t ciphertext_bytes,
    const uint8_t tag[MTFS_STM32_SAES_GCM_TAG_BYTES],
    uint8_t *plaintext);

void mtfs_stm32_saes_zeroize(void *memory, size_t bytes);
const char *mtfs_stm32_saes_status_string(mtfs_stm32_saes_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_STM32_SAES_H */
