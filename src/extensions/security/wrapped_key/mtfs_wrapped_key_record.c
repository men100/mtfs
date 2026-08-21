#include "../../../mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL

#include "mtfs_wrapped_key_record.h"

#include <limits.h>
#include <string.h>

#define MTFS_WRAPPED_KEY_RECORD_CRC_OFFSET (28U)

static const uint8_t mtfs_wrapped_key_magic[8] = {
    'M', 'T', 'F', 'S', 'W', 'K', 'E', 'Y'
};

static void put_u16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
}

static void put_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
    destination[2] = (uint8_t)(value >> 16U);
    destination[3] = (uint8_t)(value >> 24U);
}

static uint16_t get_u16(const uint8_t *source)
{
    return (uint16_t)((uint16_t)source[0] |
        ((uint16_t)source[1] << 8U));
}

static uint32_t get_u32(const uint8_t *source)
{
    return (uint32_t)source[0] |
        ((uint32_t)source[1] << 8U) |
        ((uint32_t)source[2] << 16U) |
        ((uint32_t)source[3] << 24U);
}

static uint32_t crc32_update(
    uint32_t crc, const uint8_t *data, size_t data_bytes)
{
    size_t index;

    for (index = 0U; index < data_bytes; ++index) {
        unsigned int bit;
        crc ^= data[index];
        for (bit = 0U; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return crc;
}

uint32_t mtfs_wrapped_key_record_crc32(
    const void *data, size_t data_bytes)
{
    uint32_t crc = UINT32_MAX;

    if ((data == NULL) && (data_bytes != 0U)) {
        return 0U;
    }
    crc = crc32_update(crc, (const uint8_t *)data, data_bytes);
    return crc ^ UINT32_MAX;
}

size_t mtfs_wrapped_key_record_size(size_t blob_bytes)
{
    if (blob_bytes > (SIZE_MAX - MTFS_WRAPPED_KEY_RECORD_HEADER_BYTES)) {
        return 0U;
    }
    return MTFS_WRAPPED_KEY_RECORD_HEADER_BYTES + blob_bytes;
}

mtfs_wrapped_key_record_status_t mtfs_wrapped_key_record_encode(
    uint8_t *record,
    size_t record_capacity,
    const mtfs_wrapped_key_metadata_t *metadata,
    const uint8_t *blob,
    size_t blob_bytes,
    size_t *record_bytes)
{
    size_t required_bytes;
    uint32_t crc;

    if ((record == NULL) || (metadata == NULL) || (blob == NULL) ||
        (record_bytes == NULL) || (metadata->provider == 0U) ||
        (metadata->key_type == 0U) || (metadata->key_id == 0U) ||
        (metadata->key_version == 0U) || (blob_bytes == 0U) ||
        (blob_bytes > UINT32_MAX)) {
        return MTFS_WRAPPED_KEY_RECORD_INVALID_ARGUMENT;
    }
    required_bytes = mtfs_wrapped_key_record_size(blob_bytes);
    if ((required_bytes == 0U) || (record_capacity < required_bytes)) {
        return MTFS_WRAPPED_KEY_RECORD_BUFFER_TOO_SMALL;
    }

    memset(record, 0, required_bytes);
    memcpy(record, mtfs_wrapped_key_magic, sizeof(mtfs_wrapped_key_magic));
    put_u16(record + 8U, MTFS_WRAPPED_KEY_RECORD_FORMAT_VERSION);
    put_u16(record + 10U, MTFS_WRAPPED_KEY_RECORD_HEADER_BYTES);
    put_u32(record + 12U, metadata->provider);
    put_u32(record + 16U, metadata->key_type);
    put_u32(record + 20U, metadata->key_id);
    put_u32(record + 24U, metadata->key_version);
    put_u32(record + MTFS_WRAPPED_KEY_RECORD_CRC_OFFSET, 0U);
    memcpy(record + MTFS_WRAPPED_KEY_RECORD_HEADER_BYTES, blob, blob_bytes);

    crc = UINT32_MAX;
    crc = crc32_update(crc, record, MTFS_WRAPPED_KEY_RECORD_CRC_OFFSET);
    crc = crc32_update(crc,
        record + MTFS_WRAPPED_KEY_RECORD_HEADER_BYTES, blob_bytes);
    put_u32(record + MTFS_WRAPPED_KEY_RECORD_CRC_OFFSET,
        crc ^ UINT32_MAX);
    *record_bytes = required_bytes;
    return MTFS_WRAPPED_KEY_RECORD_OK;
}

mtfs_wrapped_key_record_status_t mtfs_wrapped_key_record_decode(
    const uint8_t *record,
    size_t record_bytes,
    uint32_t expected_provider,
    uint32_t expected_key_type,
    size_t expected_blob_bytes,
    mtfs_wrapped_key_metadata_t *metadata,
    const uint8_t **blob)
{
    mtfs_wrapped_key_metadata_t decoded;
    uint32_t stored_crc;
    uint32_t calculated_crc;

    if ((record == NULL) || (metadata == NULL) || (blob == NULL) ||
        (expected_provider == 0U) || (expected_key_type == 0U) ||
        (expected_blob_bytes == 0U)) {
        return MTFS_WRAPPED_KEY_RECORD_INVALID_ARGUMENT;
    }
    if (record_bytes < MTFS_WRAPPED_KEY_RECORD_HEADER_BYTES) {
        return MTFS_WRAPPED_KEY_RECORD_INVALID_FORMAT;
    }
    if ((memcmp(record, mtfs_wrapped_key_magic,
            sizeof(mtfs_wrapped_key_magic)) != 0) ||
        (get_u16(record + 8U) != MTFS_WRAPPED_KEY_RECORD_FORMAT_VERSION) ||
        (get_u16(record + 10U) != MTFS_WRAPPED_KEY_RECORD_HEADER_BYTES) ||
        (record_bytes != mtfs_wrapped_key_record_size(expected_blob_bytes))) {
        return MTFS_WRAPPED_KEY_RECORD_INVALID_FORMAT;
    }

    decoded.provider = get_u32(record + 12U);
    decoded.key_type = get_u32(record + 16U);
    decoded.key_id = get_u32(record + 20U);
    decoded.key_version = get_u32(record + 24U);
    if ((decoded.provider != expected_provider) ||
        (decoded.key_type != expected_key_type)) {
        return MTFS_WRAPPED_KEY_RECORD_UNSUPPORTED;
    }
    if ((decoded.key_id == 0U) || (decoded.key_version == 0U)) {
        return MTFS_WRAPPED_KEY_RECORD_INVALID_FORMAT;
    }

    stored_crc = get_u32(record + MTFS_WRAPPED_KEY_RECORD_CRC_OFFSET);
    calculated_crc = UINT32_MAX;
    calculated_crc = crc32_update(calculated_crc, record,
        MTFS_WRAPPED_KEY_RECORD_CRC_OFFSET);
    calculated_crc = crc32_update(calculated_crc,
        record + MTFS_WRAPPED_KEY_RECORD_HEADER_BYTES,
        expected_blob_bytes);
    calculated_crc ^= UINT32_MAX;
    if (stored_crc != calculated_crc) {
        return MTFS_WRAPPED_KEY_RECORD_CRC_MISMATCH;
    }

    *metadata = decoded;
    *blob = record + MTFS_WRAPPED_KEY_RECORD_HEADER_BYTES;
    return MTFS_WRAPPED_KEY_RECORD_OK;
}

const char *mtfs_wrapped_key_record_status_string(
    mtfs_wrapped_key_record_status_t status)
{
    switch (status) {
    case MTFS_WRAPPED_KEY_RECORD_OK:
        return "ok";
    case MTFS_WRAPPED_KEY_RECORD_INVALID_ARGUMENT:
        return "invalid-argument";
    case MTFS_WRAPPED_KEY_RECORD_BUFFER_TOO_SMALL:
        return "buffer-too-small";
    case MTFS_WRAPPED_KEY_RECORD_INVALID_FORMAT:
        return "invalid-format";
    case MTFS_WRAPPED_KEY_RECORD_UNSUPPORTED:
        return "unsupported";
    case MTFS_WRAPPED_KEY_RECORD_CRC_MISMATCH:
        return "crc-mismatch";
    default:
        return "unknown";
    }
}

#else
typedef int mtfs_wrapped_key_record_disabled_translation_unit_t;
#endif /* MTFS_ENABLE_SEALED_MODEL */
