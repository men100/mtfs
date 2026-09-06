#include "mtfs_ra8p1_sentinel_runtime.h"

#include <limits.h>
#include <stddef.h>

#include <tk/tkernel.h>
#include "mbedtls/memory_buffer_alloc.h"
#include "mbedtls/platform.h"
#include "psa/crypto.h"
#include "psa/crypto_extra.h"
#include "extensions/security/sealed_blob/mtfs_secure_zero.h"
#include "mtfs_ra8p1_tflm_ethosu.h"

#define RSIP_MEMORY_BUFFER_SIZE (4096U)

typedef union rsip_memory_buffer
{
    uint64_t alignment;
    unsigned char bytes[RSIP_MEMORY_BUFFER_SIZE];
} rsip_memory_buffer_t;

static ID rsip_mutex_id;
static ID npu_mutex_id;
static mbedtls_platform_context platform_context;
static rsip_memory_buffer_t rsip_memory_buffer;
static uint8_t platform_ready;
static uint8_t allocator_ready;
static int32_t rsip_last_lock_status;
static int32_t rsip_last_unlock_status;
static uint32_t rsip_lock_attempts;
static uint32_t rsip_unlock_attempts;

static void delete_mutex(ID *mutex_id)
{
    if (*mutex_id > 0) (void)tk_del_mtx(*mutex_id);
    *mutex_id = 0;
}

int mtfs_ra8p1_sentinel_runtime_init(void)
{
    T_CMTX mutex = {.mtxatr = TA_INHERIT};
    psa_status_t psa_status;
    if (rsip_mutex_id > 0 && npu_mutex_id > 0 && platform_ready != 0U &&
        allocator_ready != 0U)
        return 0;
    if (rsip_mutex_id != 0 || npu_mutex_id != 0 || platform_ready != 0U ||
        allocator_ready != 0U)
        return -1;
    rsip_last_lock_status = 0;
    rsip_last_unlock_status = 0;
    rsip_lock_attempts = 0U;
    rsip_unlock_attempts = 0U;
    rsip_mutex_id = tk_cre_mtx(&mutex);
    npu_mutex_id = tk_cre_mtx(&mutex);
    if (rsip_mutex_id <= 0 || npu_mutex_id <= 0) goto fail;
    if (mbedtls_platform_setup(&platform_context) != 0) goto fail;
    platform_ready = 1U;
    mbedtls_memory_buffer_alloc_init(rsip_memory_buffer.bytes,
        sizeof(rsip_memory_buffer.bytes));
    allocator_ready = 1U;
    if (mbedtls_memory_buffer_alloc_verify() != 0) goto fail;
    if (tk_loc_mtx(rsip_mutex_id, TMO_FEVR) != E_OK) goto fail;
    psa_status = psa_crypto_init();
    (void)tk_unl_mtx(rsip_mutex_id);
    if (psa_status != PSA_SUCCESS) goto fail;
    return 0;
fail:
    if (allocator_ready != 0U)
        mbedtls_memory_buffer_alloc_free();
    allocator_ready = 0U;
    mtfs_secure_zero(&rsip_memory_buffer, sizeof(rsip_memory_buffer));
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
    if (rsip_mutex_id > 0 && allocator_ready != 0U &&
        tk_loc_mtx(rsip_mutex_id, TMO_FEVR) == E_OK) {
        mbedtls_psa_crypto_free();
        (void)tk_unl_mtx(rsip_mutex_id);
    }
    if (allocator_ready != 0U)
        mbedtls_memory_buffer_alloc_free();
    allocator_ready = 0U;
    mtfs_secure_zero(&rsip_memory_buffer, sizeof(rsip_memory_buffer));
    if (platform_ready != 0U)
        mbedtls_platform_teardown(&platform_context);
    platform_ready = 0U;
    delete_mutex(&npu_mutex_id);
    delete_mutex(&rsip_mutex_id);
}

int mtfs_ra8p1_sentinel_rsip_lock(void *context)
{
    ER status;
    (void)context;
    ++rsip_lock_attempts;
    if (rsip_mutex_id <= 0) {
        rsip_last_lock_status = -1;
        return -1;
    }
    status = tk_loc_mtx(rsip_mutex_id, TMO_FEVR);
    rsip_last_lock_status = (int32_t)status;
    return status == E_OK ? 0 : -1;
}

void mtfs_ra8p1_sentinel_rsip_unlock(void *context)
{
    ER status;
    (void)context;
    ++rsip_unlock_attempts;
    if (rsip_mutex_id <= 0) {
        rsip_last_unlock_status = -1;
        return;
    }
    status = tk_unl_mtx(rsip_mutex_id);
    rsip_last_unlock_status = (int32_t)status;
}

void mtfs_ra8p1_sentinel_rsip_lock_diagnostics(int32_t *last_lock_status,
    int32_t *last_unlock_status, uint32_t *lock_attempts,
    uint32_t *unlock_attempts)
{
    if (last_lock_status != NULL) *last_lock_status = rsip_last_lock_status;
    if (last_unlock_status != NULL)
        *last_unlock_status = rsip_last_unlock_status;
    if (lock_attempts != NULL) *lock_attempts = rsip_lock_attempts;
    if (unlock_attempts != NULL) *unlock_attempts = rsip_unlock_attempts;
}

void mtfs_ra8p1_sentinel_rsip_memory_diagnostics(uint32_t *capacity,
    int *verified)
{
    if (capacity != NULL) *capacity = RSIP_MEMORY_BUFFER_SIZE;
    if (verified != NULL) *verified = allocator_ready != 0U &&
        mbedtls_memory_buffer_alloc_verify() == 0;
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
