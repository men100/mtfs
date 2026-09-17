/** @file mtfs_wrapped_key_record.h
 * @brief Portable wrapped fleet-key record encoding. / portableなwrapped fleet key recordのencoding。
 * @ingroup mtfs_sealed */
#ifndef MTFS_WRAPPED_KEY_RECORD_H
#define MTFS_WRAPPED_KEY_RECORD_H

/** @addtogroup mtfs_sealed
 * @{ */

#include "../../../mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_WRAPPED_KEY_RECORD_FORMAT_VERSION (1U)
#define MTFS_WRAPPED_KEY_RECORD_HEADER_BYTES    (32U)
#define MTFS_WRAPPED_KEY_RECORD_RSIP_AES256_BLOB_BYTES (52U)
#define MTFS_WRAPPED_KEY_RECORD_RSIP_AES256_BYTES \
    (MTFS_WRAPPED_KEY_RECORD_HEADER_BYTES + \
     MTFS_WRAPPED_KEY_RECORD_RSIP_AES256_BLOB_BYTES)
#define MTFS_WRAPPED_KEY_RECORD_SAES_AES256_BLOB_BYTES (32U)
#define MTFS_WRAPPED_KEY_RECORD_SAES_AES256_BYTES \
    (MTFS_WRAPPED_KEY_RECORD_HEADER_BYTES + \
     MTFS_WRAPPED_KEY_RECORD_SAES_AES256_BLOB_BYTES)

typedef enum mtfs_wrapped_key_provider
{
    MTFS_WRAPPED_KEY_PROVIDER_RA_RSIP_E50D = 1,
    MTFS_WRAPPED_KEY_PROVIDER_STM32_SAES_DHUK = 2
} mtfs_wrapped_key_provider_t;

typedef enum mtfs_wrapped_key_type
{
    MTFS_WRAPPED_KEY_TYPE_AES_256 = 1
} mtfs_wrapped_key_type_t;

typedef enum mtfs_wrapped_key_record_status
{
    MTFS_WRAPPED_KEY_RECORD_OK = 0,
    MTFS_WRAPPED_KEY_RECORD_INVALID_ARGUMENT = -1,
    MTFS_WRAPPED_KEY_RECORD_BUFFER_TOO_SMALL = -2,
    MTFS_WRAPPED_KEY_RECORD_INVALID_FORMAT = -3,
    MTFS_WRAPPED_KEY_RECORD_UNSUPPORTED = -4,
    MTFS_WRAPPED_KEY_RECORD_CRC_MISMATCH = -5
} mtfs_wrapped_key_record_status_t;

typedef struct mtfs_wrapped_key_metadata
{
    uint32_t provider;
    uint32_t key_type;
    uint32_t key_id;
    uint32_t key_version;
} mtfs_wrapped_key_metadata_t;

/** @brief Compute the record CRC-32 integrity check. / recordの破損検出に使用するCRC-32を計算する。
 * @param data Input bytes. / 入力データ。
 * @param data_bytes Input size. / 入力データのサイズ（byte単位）。
 * @return CRC-32 value. / CRC-32値。
 * @warning CRC detects corruption; it is not cryptographic authentication. / CRCはデータ破損の検出用であり、暗号学的な認証機能は提供しない。 */
uint32_t mtfs_wrapped_key_record_crc32(
    const void *data, size_t data_bytes);

/** @brief Return header plus blob size, or zero on overflow. / headerとblobを含むrecord全体のサイズを返す。overflow時は0を返す。
 * @param blob_bytes Wrapped blob bytes. / wrapped blobのサイズ（byte単位）。
 * @return Required record bytes or zero. / 必要なrecordサイズ。overflow時は0。 */
size_t mtfs_wrapped_key_record_size(size_t blob_bytes);

/** @brief Encode metadata and an opaque hardware-wrapped blob. / metadataと、内部内容を直接扱わないhardware-wrapped blobをrecordにencodeする。
 * @param[out] record Destination. / encode先のrecord領域。
 * @param record_capacity Available bytes. / record領域の利用可能サイズ（byte単位）。
 * @param metadata Provider, type, id, and version. / provider、key type、key id、およびversion。
 * @param blob Opaque wrapped-key bytes; plaintext keys are not accepted. / 内部内容を直接扱わないwrapped key data。plaintext keyは受け付けない。
 * @param blob_bytes Blob size. / blobのサイズ（byte単位）。
 * @param[out] record_bytes Encoded size. / encode後のrecordサイズ。
 * @return Record status. / record処理のstatus。 */
mtfs_wrapped_key_record_status_t mtfs_wrapped_key_record_encode(
    uint8_t *record,
    size_t record_capacity,
    const mtfs_wrapped_key_metadata_t *metadata,
    const uint8_t *blob,
    size_t blob_bytes,
    size_t *record_bytes);

/** @brief Validate and expose a borrowed wrapped-blob view. / recordを検証し、その内部を参照する非所有のwrapped blob viewを返す。
 * @param record Complete record retained while blob is used. / blobの使用中、有効な状態を維持するcomplete record。
 * @param record_bytes Record bytes. / recordのサイズ（byte単位）。
 * @param expected_provider Required provider id. / recordに必須のprovider id。
 * @param expected_key_type Required key type. / recordに必須のkey type。
 * @param expected_blob_bytes Exact required blob size. / recordに必須のblobサイズ。
 * @param[out] metadata Decoded metadata. / decodeしたmetadata。
 * @param[out] blob Borrowed pointer into record. / record内部を参照する非所有pointer。
 * @return Record status including CRC_MISMATCH. / CRC_MISMATCHを含むrecord処理のstatus。 */
mtfs_wrapped_key_record_status_t mtfs_wrapped_key_record_decode(
    const uint8_t *record,
    size_t record_bytes,
    uint32_t expected_provider,
    uint32_t expected_key_type,
    size_t expected_blob_bytes,
    mtfs_wrapped_key_metadata_t *metadata,
    const uint8_t **blob);

const char *mtfs_wrapped_key_record_status_string(
    mtfs_wrapped_key_record_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_ENABLE_SEALED_MODEL */

/** @} */

#endif /* MTFS_WRAPPED_KEY_RECORD_H */
