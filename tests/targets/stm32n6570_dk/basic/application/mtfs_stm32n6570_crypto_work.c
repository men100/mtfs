#include "mtfs_stm32n6570_crypto_work.h"

#if defined(__GNUC__)
#define TEST_WORK_ALIGN __attribute__((aligned(32)))
#else
#define TEST_WORK_ALIGN
#endif

uint8_t mtfs_stm32n6570_test_plaintext_work[
    MTFS_SEALED_MAX_CHUNK_SIZE] TEST_WORK_ALIGN;
uint8_t mtfs_stm32n6570_test_ciphertext_work[
    MTFS_SEALED_CIPHER_BUFFER_SIZE] TEST_WORK_ALIGN;
