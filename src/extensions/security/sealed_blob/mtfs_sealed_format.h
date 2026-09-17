/** @file mtfs_sealed_format.h
 * @brief Versioned sealed-package format parser. / version付きsealed package formatのparser。
 * @ingroup mtfs_sealed */
#ifndef MTFS_SEALED_FORMAT_H
#define MTFS_SEALED_FORMAT_H

/** @addtogroup mtfs_sealed
 * @{ */

#include "mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL
#include <stddef.h>
#include <stdint.h>
#include "mtfs_error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_SEALED_FORMAT_API_VERSION (1U)
#define MTFS_SEALED_OBJECT_TYPE_AI_MODEL (1U)
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
    uint32_t object_type;
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

/** @brief Parse and validate the fixed preamble without authenticating it. / 固定preambleをparseして形式を検証する。この時点では認証は行わない。
 * @param preamble Exactly MTFS_SEALED_PREAMBLE_SIZE bytes. / MTFS_SEALED_PREAMBLE_SIZE byte固定のpreamble。
 * @param[out] info Parsed package metadata. / parseしたpackage metadata。
 * @param[out] layout Parsed offsets and key identifiers. / parseしたoffsetとkey identifier。
 * @return MTFS_OK or format/range/argument error. / MTFS_OKまたはformat/範囲/引数error。
 * @warning Parsed values are untrusted until the manifest is authenticated. / manifestの認証が完了するまでは、parseした値を信頼してはならない。 */
mtfs_error_t mtfs_sealed_format_parse(const uint8_t preamble[160],
    mtfs_sealed_package_info_t *info, mtfs_sealed_layout_t *layout);

/** @brief Validate bounded opaque metadata encoding. / サイズ上限付きのopaque metadata encodingを検証する。
 * @param metadata Metadata bytes, or NULL only when size is zero. / metadata。sizeが0の場合のみNULL指定可。
 * @param metadata_size Bytes not exceeding MTFS_SEALED_MAX_METADATA_SIZE. / metadataのサイズ。MTFS_SEALED_MAX_METADATA_SIZE以下であること。
 * @return MTFS_OK or validation error. / MTFS_OKまたはvalidation error。 */
mtfs_error_t mtfs_sealed_metadata_validate(const uint8_t *metadata,
    size_t metadata_size);

/** @brief Derive checked payload offsets and expected file size. / 検証済みのpayload offsetと想定file sizeを算出する。
 * @param info Previously parsed package information. / 事前にparse済みのpackage情報。
 * @param[in,out] layout Parsed layout completed in place. / parse済みlayoutに残りの情報を設定して完成させる。
 * @return MTFS_OK or overflow/format/argument error. / MTFS_OKまたはoverflow/format/引数error。 */
mtfs_error_t mtfs_sealed_format_finish_layout(
    const mtfs_sealed_package_info_t *info, mtfs_sealed_layout_t *layout);

#ifdef __cplusplus
}
#endif
#endif /* MTFS_ENABLE_SEALED_MODEL */

/** @} */

#endif /* MTFS_SEALED_FORMAT_H */
