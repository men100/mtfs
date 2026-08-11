/* Common public types for microT-FS. */
#ifndef MTFS_TYPES_H
#define MTFS_TYPES_H

#include <stdint.h>

typedef uint64_t mtfs_lba_t;

/* Public object definitions remain private until the core API is established. */
typedef struct mtfs_context mtfs_context_t;
typedef struct mtfs_block_device mtfs_block_device_t;

#endif /* MTFS_TYPES_H */
