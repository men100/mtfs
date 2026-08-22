#include "mtfs_sealed_format.h"

#if MTFS_ENABLE_SEALED_MODEL

#include <limits.h>
#include <string.h>

static uint16_t mtfs_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t mtfs_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t mtfs_get_u64(const uint8_t *p)
{
    return (uint64_t)mtfs_get_u32(p) |
           ((uint64_t)mtfs_get_u32(p + 4) << 32);
}

static int mtfs_all_zero(const uint8_t *p, size_t size)
{
    uint8_t value = 0U;
    size_t i;
    for (i = 0U; i < size; ++i)
        value = (uint8_t)(value | p[i]);
    return value == 0U;
}

static int mtfs_valid_utf8_no_nul(const uint8_t *s, size_t size)
{
    size_t i = 0U;
    while (i < size)
    {
        uint32_t code;
        size_t continuation;
        uint8_t first = s[i++];
        if (first == 0U)
            return 0;
        if (first < 0x80U)
            continue;
        if (first >= 0xC2U && first <= 0xDFU)
        {
            code = (uint32_t)(first & 0x1FU);
            continuation = 1U;
        }
        else if (first >= 0xE0U && first <= 0xEFU)
        {
            code = (uint32_t)(first & 0x0FU);
            continuation = 2U;
        }
        else if (first >= 0xF0U && first <= 0xF4U)
        {
            code = (uint32_t)(first & 0x07U);
            continuation = 3U;
        }
        else
            return 0;
        if (continuation > size - i)
            return 0;
        while (continuation-- != 0U)
        {
            uint8_t next = s[i++];
            if ((next & 0xC0U) != 0x80U)
                return 0;
            code = (code << 6) | (uint32_t)(next & 0x3FU);
        }
        if ((code < 0x80U) || (code < 0x800U && first >= 0xE0U) ||
            (code < 0x10000U && first >= 0xF0U) ||
            (code >= 0xD800U && code <= 0xDFFFU) || code > 0x10FFFFU)
            return 0;
    }
    return 1;
}

static int mtfs_valid_chunk_size(uint32_t size)
{
    return size >= MTFS_SEALED_MIN_CHUNK_SIZE &&
           size <= MTFS_SEALED_MAX_CHUNK_SIZE && (size & (size - 1U)) == 0U;
}

mtfs_error_t mtfs_sealed_metadata_validate(const uint8_t *metadata,
                                            size_t metadata_size)
{
    size_t offset = 0U;
    uint16_t previous_type = 0U;
    if (metadata_size > MTFS_SEALED_MAX_METADATA_SIZE ||
        (metadata == NULL && metadata_size != 0U))
        return MTFS_ERROR_MALFORMED_FORMAT;
    while (offset < metadata_size)
    {
        uint16_t type;
        uint16_t flags;
        uint32_t length;
        size_t raw_size;
        size_t padded_size;
        size_t i;
        const uint8_t *value;
        if (metadata_size - offset < 8U)
            return MTFS_ERROR_MALFORMED_FORMAT;
        type = mtfs_get_u16(metadata + offset);
        flags = mtfs_get_u16(metadata + offset + 2U);
        length = mtfs_get_u32(metadata + offset + 4U);
        if (type == 0U || type <= previous_type || (flags & (uint16_t)~1U) != 0U)
            return MTFS_ERROR_MALFORMED_FORMAT;
        if ((uint64_t)length > (uint64_t)(metadata_size - offset - 8U))
            return MTFS_ERROR_MALFORMED_FORMAT;
        value = metadata + offset + 8U;
        switch (type)
        {
        case 1U:
            if (length != 4U)
                return MTFS_ERROR_MALFORMED_FORMAT;
            break;
        case 2U:
            if (length != 8U)
                return MTFS_ERROR_MALFORMED_FORMAT;
            break;
        case 3U:
            if (length < 1U || length > 1024U ||
                !mtfs_valid_utf8_no_nul(value, (size_t)length))
                return MTFS_ERROR_MALFORMED_FORMAT;
            break;
        case 4U:
            if (length < 1U || length > 255U ||
                !mtfs_valid_utf8_no_nul(value, (size_t)length))
                return MTFS_ERROR_MALFORMED_FORMAT;
            break;
        default:
            if ((flags & 1U) != 0U)
                return MTFS_ERROR_UNSUPPORTED_FORMAT;
            break;
        }
        raw_size = 8U + (size_t)length;
        if (raw_size > SIZE_MAX - 7U)
            return MTFS_ERROR_OVERFLOW;
        padded_size = (raw_size + 7U) & ~(size_t)7U;
        if (padded_size > metadata_size - offset)
            return MTFS_ERROR_MALFORMED_FORMAT;
        for (i = raw_size; i < padded_size; ++i)
            if (metadata[offset + i] != 0U)
                return MTFS_ERROR_MALFORMED_FORMAT;
        offset += padded_size;
        previous_type = type;
    }
    return MTFS_OK;
}

mtfs_error_t mtfs_sealed_format_finish_layout(
    const mtfs_sealed_package_info_t *info, mtfs_sealed_layout_t *layout)
{
    uint64_t expected_chunks = 0U;
    uint64_t value;
    if (info == NULL || layout == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (info->payload_plain_length != 0U)
    {
        if (info->payload_plain_length > UINT64_MAX - (info->chunk_plain_size - 1U))
            return MTFS_ERROR_OVERFLOW;
        value = info->payload_plain_length + (info->chunk_plain_size - 1U);
        expected_chunks = value / info->chunk_plain_size;
    }
    if (expected_chunks > UINT32_MAX || info->chunk_count != (uint32_t)expected_chunks)
        return MTFS_ERROR_MALFORMED_FORMAT;
    value = (uint64_t)info->chunk_count * MTFS_SEALED_TAG_SIZE;
    if (info->payload_plain_length > UINT64_MAX - value)
        return MTFS_ERROR_OVERFLOW;
    value += info->payload_plain_length;
    if (layout->payload_offset > UINT64_MAX - value)
        return MTFS_ERROR_OVERFLOW;
    layout->expected_file_size = layout->payload_offset + value;
    return MTFS_OK;
}

mtfs_error_t mtfs_sealed_format_parse(const uint8_t preamble[160],
                                      mtfs_sealed_package_info_t *info,
                                      mtfs_sealed_layout_t *layout)
{
    static const uint8_t magic[8] = {'M','T','F','S','M','O','D',0};
    uint32_t manifest_size;
    uint32_t metadata_size;
    if (preamble == NULL || info == NULL || layout == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    memset(info, 0, sizeof(*info));
    memset(layout, 0, sizeof(*layout));
    info->api_version = MTFS_SEALED_FORMAT_API_VERSION;
    info->struct_size = (uint32_t)sizeof(*info);
    layout->api_version = MTFS_SEALED_FORMAT_API_VERSION;
    layout->struct_size = (uint32_t)sizeof(*layout);
    if (memcmp(preamble, magic, sizeof(magic)) != 0 ||
        mtfs_get_u32(preamble + 12U) != MTFS_SEALED_PREAMBLE_SIZE)
        return MTFS_ERROR_MALFORMED_FORMAT;
    if (mtfs_get_u16(preamble + 8U) != 1U || mtfs_get_u16(preamble + 10U) != 0U)
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    manifest_size = mtfs_get_u32(preamble + 16U);
    metadata_size = mtfs_get_u32(preamble + 120U);
    if (metadata_size > MTFS_SEALED_MAX_METADATA_SIZE ||
        manifest_size != MTFS_SEALED_PREAMBLE_SIZE + metadata_size ||
        mtfs_get_u32(preamble + 124U) != MTFS_SEALED_ENVELOPE_SIZE ||
        !mtfs_all_zero(preamble + 92U, 4U) ||
        !mtfs_all_zero(preamble + 148U, 12U) ||
        mtfs_all_zero(preamble + 40U, 16U) ||
        mtfs_all_zero(preamble + 128U, 12U) ||
        mtfs_all_zero(preamble + 140U, 8U))
        return MTFS_ERROR_MALFORMED_FORMAT;
    if (mtfs_get_u32(preamble + 20U) != 1U ||
        mtfs_get_u32(preamble + 24U) != 1U ||
        mtfs_get_u16(preamble + 28U) != 1U ||
        mtfs_get_u16(preamble + 30U) != 1U ||
        mtfs_get_u32(preamble + 32U) != 1U ||
        mtfs_get_u32(preamble + 36U) != 1U)
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    memcpy(info->package_id, preamble + 40U, 16U);
    memcpy(info->model_id, preamble + 56U, 16U);
    info->model_version = mtfs_get_u64(preamble + 72U);
    info->target_id = mtfs_get_u32(preamble + 80U);
    info->accelerator_id = mtfs_get_u32(preamble + 84U);
    info->model_format = mtfs_get_u32(preamble + 88U);
    info->required_ram = mtfs_get_u64(preamble + 96U);
    info->payload_plain_length = mtfs_get_u64(preamble + 104U);
    info->chunk_plain_size = mtfs_get_u32(preamble + 112U);
    info->chunk_count = mtfs_get_u32(preamble + 116U);
    info->metadata_size = metadata_size;
    if (!mtfs_valid_chunk_size(info->chunk_plain_size) ||
        info->required_ram < info->payload_plain_length)
        return MTFS_ERROR_MALFORMED_FORMAT;
    layout->manifest_size = manifest_size;
    layout->metadata_size = metadata_size;
    layout->envelope_offset = manifest_size;
    layout->payload_offset = (uint64_t)manifest_size + MTFS_SEALED_ENVELOPE_SIZE;
    layout->key_id = mtfs_get_u32(preamble + 32U);
    layout->key_version = mtfs_get_u32(preamble + 36U);
    memcpy(layout->key_nonce, preamble + 128U, 12U);
    memcpy(layout->payload_nonce_prefix, preamble + 140U, 8U);
    return mtfs_sealed_format_finish_layout(info, layout);
}

#else
typedef int mtfs_sealed_format_disabled_translation_unit_t;
#endif /* MTFS_ENABLE_SEALED_MODEL */
