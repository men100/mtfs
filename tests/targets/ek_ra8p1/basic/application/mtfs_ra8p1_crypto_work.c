#include "mtfs_ra8p1_crypto_work.h"

#include <tk/tkernel.h>
#include "mbedtls/platform.h"
#include "psa/crypto.h"

#if defined(__GNUC__)
#define CRYPTO_WORK_ALIGN __attribute__((aligned(32)))
#else
#define CRYPTO_WORK_ALIGN
#endif

uint8_t mtfs_ra8p1_test_plaintext_work[
    MTFS_SEALED_MAX_CHUNK_SIZE + MTFS_SEALED_TAG_SIZE] CRYPTO_WORK_ALIGN;
uint8_t mtfs_ra8p1_test_ciphertext_work[
    MTFS_SEALED_CIPHER_BUFFER_SIZE] CRYPTO_WORK_ALIGN;

static ID rsip_mutex_id;
static mbedtls_platform_context platform_context;
static uint8_t platform_ready;

int mtfs_ra8p1_rsip_lock_init(void)
{
    T_CMTX mutex = {.mtxatr = TA_INHERIT};
    if (rsip_mutex_id > 0)
        return 0;
    rsip_mutex_id = tk_cre_mtx(&mutex);
    if (rsip_mutex_id <= 0)
        return -1;
    if (mbedtls_platform_setup(&platform_context) != 0) {
        (void)tk_del_mtx(rsip_mutex_id);
        rsip_mutex_id = 0;
        return -1;
    }
    platform_ready = 1U;
    if (tk_loc_mtx(rsip_mutex_id, TMO_FEVR) != E_OK) {
        mbedtls_platform_teardown(&platform_context);
        platform_ready = 0U;
        (void)tk_del_mtx(rsip_mutex_id);
        rsip_mutex_id = 0;
        return -1;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        (void)tk_unl_mtx(rsip_mutex_id);
        mbedtls_platform_teardown(&platform_context);
        platform_ready = 0U;
        (void)tk_del_mtx(rsip_mutex_id);
        rsip_mutex_id = 0;
        return -1;
    }
    (void)tk_unl_mtx(rsip_mutex_id);
    return 0;
}

void mtfs_ra8p1_rsip_lock_deinit(void)
{
    if (platform_ready != 0U)
        mbedtls_platform_teardown(&platform_context);
    platform_ready = 0U;
    if (rsip_mutex_id > 0)
        (void)tk_del_mtx(rsip_mutex_id);
    rsip_mutex_id = 0;
}

int mtfs_ra8p1_rsip_lock(void *context)
{
    (void)context;
    return rsip_mutex_id > 0 &&
        tk_loc_mtx(rsip_mutex_id, TMO_FEVR) == E_OK ? 0 : -1;
}

void mtfs_ra8p1_rsip_unlock(void *context)
{
    (void)context;
    if (rsip_mutex_id > 0)
        (void)tk_unl_mtx(rsip_mutex_id);
}
