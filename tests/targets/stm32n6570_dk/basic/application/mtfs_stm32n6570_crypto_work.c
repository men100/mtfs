#include "mtfs_stm32n6570_crypto_work.h"

#include <tk/tkernel.h>

#if defined(__GNUC__)
#define TEST_WORK_ALIGN __attribute__((aligned(32)))
#else
#define TEST_WORK_ALIGN
#endif

uint8_t mtfs_stm32n6570_test_plaintext_work[
    MTFS_SEALED_MAX_CHUNK_SIZE] TEST_WORK_ALIGN;
uint8_t mtfs_stm32n6570_test_ciphertext_work[
    MTFS_SEALED_CIPHER_BUFFER_SIZE] TEST_WORK_ALIGN;

static ID saes_mutex_id;

int mtfs_stm32n6570_saes_lock_init(void)
{
    T_CMTX mutex = {.mtxatr = TA_INHERIT};
    if (saes_mutex_id > 0)
        return 0;
    saes_mutex_id = tk_cre_mtx(&mutex);
    return saes_mutex_id > 0 ? 0 : -1;
}

void mtfs_stm32n6570_saes_lock_deinit(void)
{
    if (saes_mutex_id > 0)
        (void)tk_del_mtx(saes_mutex_id);
    saes_mutex_id = 0;
}

int mtfs_stm32n6570_saes_lock(void *context)
{
    (void)context;
    return saes_mutex_id > 0 &&
        tk_loc_mtx(saes_mutex_id, TMO_FEVR) == E_OK ? 0 : -1;
}

void mtfs_stm32n6570_saes_unlock(void *context)
{
    (void)context;
    if (saes_mutex_id > 0)
        (void)tk_unl_mtx(saes_mutex_id);
}
