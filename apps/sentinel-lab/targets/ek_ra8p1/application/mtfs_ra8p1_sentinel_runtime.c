#include "mtfs_ra8p1_sentinel_runtime.h"

#include <limits.h>

#include <tk/tkernel.h>
#include "mbedtls/platform.h"
#include "psa/crypto.h"
#include "mtfs_ra8p1_tflm_ethosu.h"

static ID rsip_mutex_id;
static ID npu_mutex_id;
static mbedtls_platform_context platform_context;
static uint8_t platform_ready;

static void delete_mutex(ID *mutex_id)
{
    if (*mutex_id > 0) (void)tk_del_mtx(*mutex_id);
    *mutex_id = 0;
}

int mtfs_ra8p1_sentinel_runtime_init(void)
{
    T_CMTX mutex = {.mtxatr = TA_INHERIT};
    psa_status_t psa_status;
    if (rsip_mutex_id > 0 && npu_mutex_id > 0 && platform_ready != 0U)
        return 0;
    if (rsip_mutex_id != 0 || npu_mutex_id != 0 || platform_ready != 0U)
        return -1;
    rsip_mutex_id = tk_cre_mtx(&mutex);
    npu_mutex_id = tk_cre_mtx(&mutex);
    if (rsip_mutex_id <= 0 || npu_mutex_id <= 0) goto fail;
    if (mbedtls_platform_setup(&platform_context) != 0) goto fail;
    platform_ready = 1U;
    if (tk_loc_mtx(rsip_mutex_id, TMO_FEVR) != E_OK) goto fail;
    psa_status = psa_crypto_init();
    (void)tk_unl_mtx(rsip_mutex_id);
    if (psa_status != PSA_SUCCESS) goto fail;
    return 0;
fail:
    if (platform_ready != 0U)
        mbedtls_platform_teardown(&platform_context);
    platform_ready = 0U;
    delete_mutex(&npu_mutex_id);
    delete_mutex(&rsip_mutex_id);
    return -1;
}

void mtfs_ra8p1_sentinel_runtime_shutdown(void)
{
    if (mtfs_ra8p1_tflm_ethosu_global_shutdown() != MTFS_OK) return;
    if (platform_ready != 0U)
        mbedtls_platform_teardown(&platform_context);
    platform_ready = 0U;
    delete_mutex(&npu_mutex_id);
    delete_mutex(&rsip_mutex_id);
}

int mtfs_ra8p1_sentinel_rsip_lock(void *context)
{
    (void)context;
    return rsip_mutex_id > 0 && tk_loc_mtx(rsip_mutex_id, TMO_FEVR) == E_OK ?
        0 : -1;
}

void mtfs_ra8p1_sentinel_rsip_unlock(void *context)
{
    (void)context;
    if (rsip_mutex_id > 0) (void)tk_unl_mtx(rsip_mutex_id);
}

mtfs_error_t mtfs_ra8p1_sentinel_npu_lock(void *context,
    uint32_t timeout_ms)
{
    TMO timeout;
    (void)context;
    if (npu_mutex_id <= 0 || timeout_ms == 0U) return MTFS_ERROR_NOT_READY;
    timeout = timeout_ms > (uint32_t)INT32_MAX ? TMO_FEVR : (TMO)timeout_ms;
    return tk_loc_mtx(npu_mutex_id, timeout) == E_OK ? MTFS_OK :
        MTFS_ERROR_NOT_READY;
}

void mtfs_ra8p1_sentinel_npu_unlock(void *context)
{
    (void)context;
    if (npu_mutex_id > 0) (void)tk_unl_mtx(npu_mutex_id);
}
