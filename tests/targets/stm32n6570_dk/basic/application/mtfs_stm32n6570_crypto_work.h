#ifndef MTFS_STM32N6570_CRYPTO_WORK_H
#define MTFS_STM32N6570_CRYPTO_WORK_H

#include <stdint.h>

#include "mtfs_sealed_format.h"

/* Test commands are dispatched serially by the console task.  Sharing this
 * arena avoids retaining a second 128 KiB pair solely for low-level spikes. */
extern uint8_t mtfs_stm32n6570_test_plaintext_work[
    MTFS_SEALED_MAX_CHUNK_SIZE];
extern uint8_t mtfs_stm32n6570_test_ciphertext_work[
    MTFS_SEALED_CIPHER_BUFFER_SIZE];

/* One priority-inheritance mutex serializes the target's process-global SAES
 * peripheral and primitive work buffers.  It is owned for target lifetime,
 * shared by every provider session and the direct low-level crypto tests. */
int mtfs_stm32n6570_saes_lock_init(void);
void mtfs_stm32n6570_saes_lock_deinit(void);
int mtfs_stm32n6570_saes_lock(void *context);
void mtfs_stm32n6570_saes_unlock(void *context);

#endif
