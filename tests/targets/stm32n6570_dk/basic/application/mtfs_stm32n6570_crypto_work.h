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

#endif
