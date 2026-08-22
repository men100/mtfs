#ifndef MTFS_SEALED_FORMAT_H
#define MTFS_SEALED_FORMAT_H

#include "mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL
#include <stddef.h>
#include <stdint.h>
#include "mtfs_error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_SEALED_FORMAT_API_VERSION (1U)
#define MTFS_SEALED_PREAMBLE_SIZE (160U)
#define MTFS_SEALED_MAX_METADATA_SIZE (4096U)
#define MTFS_SEALED_MAX_MANIFEST_SIZE \
    (MTFS_SEALED_PREAMBLE_SIZE + MTFS_SEALED_MAX_METADATA_SIZE)
#define MTFS_SEALED_ENVELOPE_SIZE (48U)
#define MTFS_SEALED_TAG_SIZE (16U)
#define MTFS_SEALED_NONCE_SIZE (12U)
#define MTFS_SEALED_MIN_CHUNK_SIZE (4096U)
#define MTFS_SEALED_MAX_CHUNK_SIZE (65536U)
#define MTFS_SEALED_KEY_AAD_SIZE \
    (12U + MTFS_SEALED_MAX_MANIFEST_SIZE)
#define MTFS_SEALED_CHUNK_AAD_SIZE \
    (14U + MTFS_SEALED_MAX_MANIFEST_SIZE + 8U)
#define MTFS_SEALED_CIPHER_BUFFER_SIZE \
    (MTFS_SEALED_MAX_CHUNK_SIZE + MTFS_SEALED_TAG_SIZE)

typedef struct mtfs_sealed_package_info
{
    uint32_t api_version;
    uint32_t struct_size;
    uint8_t package_id[16];
    uint8_t model_id[16];
    uint64_t model_version;
    uint32_t target_id;
    uint32_t accelerator_id;
    uint32_t model_format;
    uint64_t required_ram;
    uint64_t payload_plain_length;
    uint32_t chunk_plain_size;
    uint32_t chunk_count;
    uint32_t metadata_size;
} mtfs_sealed_package_info_t;

typedef struct mtfs_sealed_layout
{
    uint32_t api_version;
    uint32_t struct_size;
    uint32_t manifest_size;
    uint32_t metadata_size;
    uint64_t envelope_offset;
    uint64_t payload_offset;
    uint64_t expected_file_size;
    uint32_t key_id;
    uint32_t key_version;
    uint8_t key_nonce[12];
    uint8_t payload_nonce_prefix[8];
} mtfs_sealed_layout_t;

mtfs_error_t mtfs_sealed_format_parse(const uint8_t preamble[160],
    mtfs_sealed_package_info_t *info, mtfs_sealed_layout_t *layout);
mtfs_error_t mtfs_sealed_metadata_validate(const uint8_t *metadata,
    size_t metadata_size);
mtfs_error_t mtfs_sealed_format_finish_layout(
    const mtfs_sealed_package_info_t *info, mtfs_sealed_layout_t *layout);

#ifdef __cplusplus
}
#endif
#endif /* MTFS_ENABLE_SEALED_MODEL */

#endif /* MTFS_SEALED_FORMAT_H */
