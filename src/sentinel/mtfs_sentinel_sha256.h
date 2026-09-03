#ifndef MTFS_SENTINEL_SHA256_H
#define MTFS_SENTINEL_SHA256_H

#include "../mtfs_config.h"

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE
#include <stddef.h>
#include <stdint.h>
#include "../mtfs_error.h"

#ifdef __cplusplus
extern "C" {
#endif

mtfs_error_t mtfs_sentinel_sha256(const void *bytes, size_t size,
    uint8_t digest[32]);

#ifdef __cplusplus
}
#endif
#endif
#endif
