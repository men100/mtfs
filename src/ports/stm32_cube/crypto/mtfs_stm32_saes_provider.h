#ifndef MTFS_STM32_SAES_PROVIDER_H
#define MTFS_STM32_SAES_PROVIDER_H

#include "../../../mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL

#include <stdint.h>

#include "../../../extensions/security/sealed_blob/mtfs_crypto_provider.h"
#include "mtfs_stm32_nor_key_store.h"
#include "mtfs_stm32_saes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_STM32_SAES_PROVIDER_API_VERSION (1U)

/* Return zero after acquiring the exclusive SAES lock, nonzero on failure. */
typedef int (*mtfs_stm32_saes_provider_lock_fn)(void *context);
typedef void (*mtfs_stm32_saes_provider_unlock_fn)(void *context);

/* Lock order contract: FatFs and SAES locks are never nested.  sealed_blob
 * completes each reader callback before invoking this provider.  Applications
 * must not hold a FatFs volume lock while calling provider functions, nor call
 * FatFs while holding the lock supplied here. */

typedef struct mtfs_stm32_saes_provider_context
{
    uint32_t api_version;
    uint32_t struct_size;
    mtfs_stm32_saes_context_t saes;
    mtfs_stm32_nor_io_t nor;
    mtfs_stm32_saes_provider_lock_fn lock;
    mtfs_stm32_saes_provider_unlock_fn unlock;
    void *lock_context;
    mtfs_stm32_saes_wrapped_key_t fleet_key;
    mtfs_stm32_saes_wrapped_key_t model_key;
    uint8_t raw_model_key[MTFS_STM32_SAES_AES256_KEY_BYTES];
    mtfs_crypto_key_handle_t fleet_handle;
    mtfs_crypto_key_handle_t model_handle;
    uint32_t handle_generation;
    mtfs_stm32_nor_key_metadata_t fleet_metadata;
    mtfs_stm32_nor_key_store_status_t last_store_status;
    mtfs_stm32_saes_status_t last_saes_status;
    uint8_t initialized;
    uint8_t fleet_open;
    uint8_t model_open;
} mtfs_stm32_saes_provider_context_t;

mtfs_crypto_status_t mtfs_stm32_saes_provider_init(
    mtfs_stm32_saes_provider_context_t *context,
    const mtfs_stm32_nor_io_t *nor,
    mtfs_stm32_saes_provider_lock_fn lock,
    mtfs_stm32_saes_provider_unlock_fn unlock,
    void *lock_context,
    mtfs_crypto_provider_t *provider);

mtfs_crypto_status_t mtfs_stm32_saes_provider_deinit(
    mtfs_stm32_saes_provider_context_t *context);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_ENABLE_SEALED_MODEL */

#endif /* MTFS_STM32_SAES_PROVIDER_H */
