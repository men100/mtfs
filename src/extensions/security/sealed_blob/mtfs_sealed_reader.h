#ifndef MTFS_SEALED_READER_H
#define MTFS_SEALED_READER_H

#include "mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL
#include <stddef.h>
#include <stdint.h>
#include "mtfs_error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_SEALED_READER_API_VERSION (1U)

typedef struct mtfs_sealed_reader
{
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    mtfs_error_t (*get_size)(void *context, uint64_t *size);
    mtfs_error_t (*read_at)(void *context, uint64_t offset, void *buffer,
                            size_t requested, size_t *read_size);
} mtfs_sealed_reader_t;

#ifdef __cplusplus
}
#endif
#endif /* MTFS_ENABLE_SEALED_MODEL */

#endif /* MTFS_SEALED_READER_H */
