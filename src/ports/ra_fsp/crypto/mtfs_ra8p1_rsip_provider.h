#ifndef MTFS_RA8P1_RSIP_PROVIDER_H
#define MTFS_RA8P1_RSIP_PROVIDER_H

#include "../../../mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL

#include <stdint.h>

#include "psa/crypto.h"
#include "r_rsip_key_injection_api.h"
#include "../../../extensions/security/sealed_blob/mtfs_crypto_provider.h"
#include "../../../extensions/security/sealed_blob/mtfs_sealed_format.h"
#include "mtfs_ra8p1_ospi_key_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_RA8P1_RSIP_PROVIDER_API_VERSION (1U)
#define MTFS_RA8P1_RSIP_PSA_TAIL_BYTES       (16U)
#if defined(__GNUC__)
#define MTFS_RA8P1_RSIP_ALIGN __attribute__((aligned(16)))
#else
#define MTFS_RA8P1_RSIP_ALIGN
#endif

/* Return zero after acquiring the target-global RSIP lock.  RSIP, PSA crypto
 * and R_RSIP_AES256_InitialKeyWrap are one target-global resource.  Every
 * provider context and every direct low-level crypto caller must share this
 * same priority-inheritance lock.  The application owns it for the complete
 * target lifetime; provider deinit never deletes it. */
typedef int (*mtfs_ra8p1_rsip_lock_fn)(void *context);
typedef void (*mtfs_ra8p1_rsip_unlock_fn)(void *context);

/* Lock order contract: FatFs and the RSIP lock are never nested.  Provider
 * functions do not call FatFs.  Applications must complete SD/FatFs reads
 * before entering a provider operation and must not call FatFs while holding
 * the lock. */

typedef union mtfs_ra8p1_rsip_model_key_work
{
    uint8_t psa_output[32U + MTFS_RA8P1_RSIP_PSA_TAIL_BYTES];
    struct
    {
        uint8_t raw_key[32U];
        uint8_t fsp_tail_guard[MTFS_RA8P1_RSIP_PSA_TAIL_BYTES];
    } fields;
} mtfs_ra8p1_rsip_model_key_work_t;

typedef struct mtfs_ra8p1_rsip_provider_context
{
    uint32_t api_version;
    uint32_t struct_size;
    mtfs_ra8p1_rsip_lock_fn lock;
    mtfs_ra8p1_rsip_unlock_fn unlock;
    void *lock_context;
    psa_key_handle_t fleet_psa_handle;
    psa_key_handle_t model_psa_handle;
    mtfs_crypto_key_handle_t fleet_handle;
    mtfs_crypto_key_handle_t model_handle;
    uint32_t handle_generation;
    mtfs_ra8p1_key_metadata_t fleet_metadata;
    mtfs_ra8p1_key_store_diagnostics_t store_diagnostics;
    int32_t last_psa_status;
    int32_t last_fsp_status;
    mtfs_ra8p1_rsip_model_key_work_t model_key_work MTFS_RA8P1_RSIP_ALIGN;
    rsip_aes_wrapped_key_t wrapped_model_key MTFS_RA8P1_RSIP_ALIGN;
    uint8_t combined[MTFS_SEALED_CIPHER_BUFFER_SIZE] MTFS_RA8P1_RSIP_ALIGN;
    uint8_t authenticated[MTFS_SEALED_MAX_CHUNK_SIZE +
        MTFS_RA8P1_RSIP_PSA_TAIL_BYTES] MTFS_RA8P1_RSIP_ALIGN;
    uint8_t initialized;
    uint8_t fleet_open;
    uint8_t model_open;
} mtfs_ra8p1_rsip_provider_context_t;

mtfs_crypto_status_t mtfs_ra8p1_rsip_provider_init(
    mtfs_ra8p1_rsip_provider_context_t *context,
    mtfs_ra8p1_rsip_lock_fn lock,
    mtfs_ra8p1_rsip_unlock_fn unlock,
    void *lock_context,
    mtfs_crypto_provider_t *provider);

mtfs_crypto_status_t mtfs_ra8p1_rsip_provider_deinit(
    mtfs_ra8p1_rsip_provider_context_t *context);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_ENABLE_SEALED_MODEL */

#endif /* MTFS_RA8P1_RSIP_PROVIDER_H */
