#ifndef MTFS_RA8P1_CRYPTO_WORK_H
#define MTFS_RA8P1_CRYPTO_WORK_H

#include <stdint.h>

#include "extensions/security/sealed_blob/mtfs_sealed_format.h"

/* The console dispatches test commands serially.  Model-store commands reuse
 * the low-level spike arena instead of retaining another pair of 64 KiB
 * buffers. */
extern uint8_t mtfs_ra8p1_test_plaintext_work[
    MTFS_SEALED_MAX_CHUNK_SIZE + MTFS_SEALED_TAG_SIZE];
extern uint8_t mtfs_ra8p1_test_ciphertext_work[
    MTFS_SEALED_CIPHER_BUFFER_SIZE];

/* One target-lifetime priority-inheritance mutex serializes RSIP hardware,
 * PSA crypto and InitialKeyWrap for all provider sessions and spike calls. */
int mtfs_ra8p1_rsip_lock_init(void);
void mtfs_ra8p1_rsip_lock_deinit(void);
int mtfs_ra8p1_rsip_lock(void *context);
void mtfs_ra8p1_rsip_unlock(void *context);

#endif
