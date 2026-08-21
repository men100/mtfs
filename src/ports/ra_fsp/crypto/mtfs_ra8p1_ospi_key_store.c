#include "../../../mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL

#include "mtfs_ra8p1_ospi_key_store.h"

#include <stddef.h>
#include <string.h>

#include "hal_data.h"

#define KEY_STORE_BASE_ADDRESS       (UINT32_C(0x90000000))
#define KEY_STORE_FORMAT_VERSION     (UINT16_C(1))
#define KEY_STORE_HEADER_BYTES       (UINT16_C(32))
#define KEY_STORE_RECORD_BYTES       (KEY_STORE_HEADER_BYTES + MTFS_RA8P1_AES256_WRAPPED_BYTES)
#define KEY_STORE_PROGRAM_PAGE_BYTES (UINT32_C(64))
#define KEY_STORE_CPU_ACCESS_BYTES   (UINT32_C(8))
#define KEY_STORE_PROGRAM_BYTES      \
    ((KEY_STORE_RECORD_BYTES + KEY_STORE_CPU_ACCESS_BYTES - 1U) & \
        ~(KEY_STORE_CPU_ACCESS_BYTES - 1U))
#define KEY_STORE_CRC_OFFSET         (28U)
#define KEY_STORE_OPERATION_TIMEOUT  (UINT32_C(2000000))

_Static_assert(KEY_STORE_RECORD_BYTES == 84U,
    "Update the OSPI key record programming layout");
_Static_assert(KEY_STORE_PROGRAM_BYTES == 88U,
    "OSPI CPU writes must be padded to 8-byte units");

static const uint8_t key_store_magic[4] = {'M', 'T', 'F', 'K'};
static uint8_t key_store_open;

static uint16_t load_u16(const uint8_t *source)
{
    return (uint16_t)((uint16_t)source[0] |
        ((uint16_t)source[1] << 8));
}

static uint32_t load_u32(const uint8_t *source)
{
    return (uint32_t)source[0] |
        ((uint32_t)source[1] << 8) |
        ((uint32_t)source[2] << 16) |
        ((uint32_t)source[3] << 24);
}

static void store_u16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}

static void store_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t bytes)
{
    uint32_t index;

    while (bytes-- != 0U) {
        crc ^= *data++;
        for (index = 0U; index < 8U; ++index) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return crc;
}

static uint32_t record_crc(const uint8_t *record)
{
    uint32_t crc = UINT32_MAX;
    static const uint8_t zero_crc[4] = {0U, 0U, 0U, 0U};

    crc = crc32_update(crc, record, KEY_STORE_CRC_OFFSET);
    crc = crc32_update(crc, zero_crc, sizeof(zero_crc));
    crc = crc32_update(crc, record + KEY_STORE_HEADER_BYTES,
        MTFS_RA8P1_AES256_WRAPPED_BYTES);
    return crc ^ UINT32_MAX;
}

void mtfs_ra8p1_ospi_key_store_zero(void *data, uint32_t bytes)
{
    volatile uint8_t *cursor = (volatile uint8_t *)data;

    while (bytes-- != 0U) {
        *cursor++ = 0U;
    }
#if defined(__GNUC__)
    __asm volatile ("" : : "r" (data) : "memory");
#endif
}

static const uint8_t *slot_address(uint32_t offset)
{
    return (const uint8_t *)(uintptr_t)(KEY_STORE_BASE_ADDRESS + offset);
}

static void invalidate_mapped_range(const void *address, uint32_t bytes)
{
#if (__DCACHE_PRESENT == 1U)
    uint32_t line_size = UINT32_C(4) <<
        ((SCB->CTR & SCB_CTR_DMINLINE_Msk) >> SCB_CTR_DMINLINE_Pos);
    uintptr_t start = (uintptr_t)address;
    uintptr_t aligned_start;
    uintptr_t aligned_end;

    if ((bytes == 0U) || ((SCB->CCR & SCB_CCR_DC_Msk) == 0U)) {
        __DSB();
        return;
    }
    aligned_start = start & ~((uintptr_t)line_size - 1U);
    aligned_end = (start + bytes + line_size - 1U) &
        ~((uintptr_t)line_size - 1U);
    SCB_InvalidateDCache_by_Addr((void *)aligned_start,
        (int32_t)(aligned_end - aligned_start));
#else
    (void)address;
    (void)bytes;
#endif
    __DSB();
}

static mtfs_ra8p1_key_store_status_t set_fsp_error(
    fsp_err_t error, mtfs_ra8p1_key_store_diagnostics_t *diagnostics)
{
    if (diagnostics != NULL) {
        diagnostics->last_fsp_error = (int32_t)error;
        diagnostics->last_status = MTFS_RA8P1_KEY_STORE_IO_ERROR;
    }
    return MTFS_RA8P1_KEY_STORE_IO_ERROR;
}

mtfs_ra8p1_key_store_status_t mtfs_ra8p1_ospi_key_store_open(
    mtfs_ra8p1_key_store_diagnostics_t *diagnostics)
{
    fsp_err_t error;

    if (diagnostics != NULL) {
        diagnostics->last_fsp_error = (int32_t)FSP_SUCCESS;
    }
    if (key_store_open != 0U) {
        return MTFS_RA8P1_KEY_STORE_OK;
    }
    error = R_OSPI_B_Open(&g_ospi0_ctrl, &g_ospi0_cfg);
    if (error != FSP_SUCCESS) {
        return set_fsp_error(error, diagnostics);
    }
    error = R_OSPI_B_SpiProtocolSet(&g_ospi0_ctrl,
        SPI_FLASH_PROTOCOL_EXTENDED_SPI);
    if (error != FSP_SUCCESS) {
        (void)R_OSPI_B_Close(&g_ospi0_ctrl);
        return set_fsp_error(error, diagnostics);
    }
    key_store_open = 1U;
    if (diagnostics != NULL) {
        diagnostics->last_status = MTFS_RA8P1_KEY_STORE_OK;
    }
    return MTFS_RA8P1_KEY_STORE_OK;
}

static int decode_record(const uint8_t *record,
    rsip_aes_wrapped_key_t *wrapped_key,
    mtfs_ra8p1_key_metadata_t *metadata,
    uint32_t slot_offset)
{
    if ((memcmp(record, key_store_magic, sizeof(key_store_magic)) != 0) ||
        (load_u16(record + 4U) != KEY_STORE_FORMAT_VERSION) ||
        (load_u16(record + 6U) != KEY_STORE_HEADER_BYTES) ||
        (load_u32(record + 20U) != MTFS_RA8P1_AES256_WRAPPED_BYTES) ||
        (load_u32(record + KEY_STORE_CRC_OFFSET) != record_crc(record))) {
        return 0;
    }
    if (wrapped_key != NULL) {
        memset(wrapped_key, 0, sizeof(*wrapped_key));
        memcpy(wrapped_key->value, record + KEY_STORE_HEADER_BYTES,
            MTFS_RA8P1_AES256_WRAPPED_BYTES);
    }
    if (metadata != NULL) {
        metadata->generation = load_u32(record + 8U);
        metadata->key_id = load_u32(record + 12U);
        metadata->key_version = load_u32(record + 16U);
        metadata->slot_offset = slot_offset;
    }
    return 1;
}

static uint32_t scan_slots(uint8_t records[2][KEY_STORE_RECORD_BYTES],
    uint8_t valid[2], mtfs_ra8p1_key_metadata_t metadata[2])
{
    static const uint32_t offsets[2] = {
        MTFS_RA8P1_OSPI_KEY_OFFSET_A,
        MTFS_RA8P1_OSPI_KEY_OFFSET_B
    };
    uint32_t index;
    uint32_t valid_count = 0U;

    for (index = 0U; index < 2U; ++index) {
        invalidate_mapped_range(slot_address(offsets[index]),
            KEY_STORE_RECORD_BYTES);
        memcpy(records[index], slot_address(offsets[index]),
            KEY_STORE_RECORD_BYTES);
        valid[index] = (uint8_t)decode_record(records[index], NULL,
            &metadata[index], offsets[index]);
        valid_count += valid[index];
    }
    return valid_count;
}

static int newest_slot(const uint8_t valid[2],
    const mtfs_ra8p1_key_metadata_t metadata[2])
{
    if ((valid[0] == 0U) && (valid[1] == 0U)) {
        return -1;
    }
    if (valid[0] == 0U) {
        return 1;
    }
    if (valid[1] == 0U) {
        return 0;
    }
    return metadata[1].generation > metadata[0].generation ? 1 : 0;
}

mtfs_ra8p1_key_store_status_t mtfs_ra8p1_ospi_key_store_load(
    rsip_aes_wrapped_key_t *wrapped_key,
    mtfs_ra8p1_key_metadata_t *metadata,
    mtfs_ra8p1_key_store_diagnostics_t *diagnostics)
{
    uint8_t records[2][KEY_STORE_RECORD_BYTES];
    uint8_t valid[2] = {0U, 0U};
    mtfs_ra8p1_key_metadata_t found[2] = {{0U}};
    uint32_t valid_count;
    int selected;
    mtfs_ra8p1_key_store_status_t status;

    if ((wrapped_key == NULL) || (metadata == NULL)) {
        return MTFS_RA8P1_KEY_STORE_INVALID_ARGUMENT;
    }
    status = mtfs_ra8p1_ospi_key_store_open(diagnostics);
    if (status != MTFS_RA8P1_KEY_STORE_OK) {
        return status;
    }
    valid_count = scan_slots(records, valid, found);
    selected = newest_slot(valid, found);
    if (diagnostics != NULL) {
        diagnostics->valid_slots = valid_count;
    }
    if (selected < 0) {
        mtfs_ra8p1_ospi_key_store_zero(records, sizeof(records));
        if (diagnostics != NULL) {
            diagnostics->last_status = MTFS_RA8P1_KEY_STORE_NOT_FOUND;
        }
        return MTFS_RA8P1_KEY_STORE_NOT_FOUND;
    }
    (void)decode_record(records[selected], wrapped_key, metadata,
        found[selected].slot_offset);
    mtfs_ra8p1_ospi_key_store_zero(records, sizeof(records));
    if (diagnostics != NULL) {
        diagnostics->last_status = MTFS_RA8P1_KEY_STORE_OK;
    }
    return MTFS_RA8P1_KEY_STORE_OK;
}

static mtfs_ra8p1_key_store_status_t wait_ready(
    mtfs_ra8p1_key_store_diagnostics_t *diagnostics)
{
    spi_flash_status_t flash_status = {0};
    uint32_t timeout = KEY_STORE_OPERATION_TIMEOUT;
    fsp_err_t error;

    do {
        error = R_OSPI_B_StatusGet(&g_ospi0_ctrl, &flash_status);
        if (error != FSP_SUCCESS) {
            return set_fsp_error(error, diagnostics);
        }
        if (!flash_status.write_in_progress) {
            return MTFS_RA8P1_KEY_STORE_OK;
        }
        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MICROSECONDS);
    } while (--timeout != 0U);
    return set_fsp_error(FSP_ERR_TIMEOUT, diagnostics);
}

mtfs_ra8p1_key_store_status_t mtfs_ra8p1_ospi_key_store_commit(
    const rsip_aes_wrapped_key_t *wrapped_key,
    uint32_t key_id,
    uint32_t key_version,
    int allow_update,
    mtfs_ra8p1_key_metadata_t *metadata,
    mtfs_ra8p1_key_store_diagnostics_t *diagnostics)
{
    static const uint32_t offsets[2] = {
        MTFS_RA8P1_OSPI_KEY_OFFSET_A,
        MTFS_RA8P1_OSPI_KEY_OFFSET_B
    };
    uint8_t records[2][KEY_STORE_RECORD_BYTES];
    uint8_t valid[2] = {0U, 0U};
    uint8_t output[KEY_STORE_PROGRAM_BYTES] __attribute__((aligned(16)));
    uint8_t verify[KEY_STORE_RECORD_BYTES] __attribute__((aligned(16)));
    mtfs_ra8p1_key_metadata_t found[2] = {{0U}};
    mtfs_ra8p1_key_metadata_t verified = {0U};
    uint32_t valid_count;
    uint32_t generation = 1U;
    uint32_t written = 0U;
    int current;
    int target;
    fsp_err_t error;
    mtfs_ra8p1_key_store_status_t status;

    if ((wrapped_key == NULL) || (metadata == NULL) || (key_id == 0U)) {
        return MTFS_RA8P1_KEY_STORE_INVALID_ARGUMENT;
    }
    status = mtfs_ra8p1_ospi_key_store_open(diagnostics);
    if (status != MTFS_RA8P1_KEY_STORE_OK) {
        return status;
    }
    valid_count = scan_slots(records, valid, found);
    current = newest_slot(valid, found);
    if (diagnostics != NULL) {
        diagnostics->valid_slots = valid_count;
    }
    if ((current >= 0) && !allow_update) {
        mtfs_ra8p1_ospi_key_store_zero(records, sizeof(records));
        if (diagnostics != NULL) {
            diagnostics->last_status =
                MTFS_RA8P1_KEY_STORE_ALREADY_PROVISIONED;
        }
        return MTFS_RA8P1_KEY_STORE_ALREADY_PROVISIONED;
    }
    if (current >= 0) {
        generation = found[current].generation + 1U;
        if (generation == 0U) {
            generation = 1U;
        }
        target = current ^ 1;
    } else {
        target = 0;
    }

    /* Padding remains erased and makes every CPU-mode write a multiple of 8. */
    memset(output, 0xff, sizeof(output));
    memset(output, 0, KEY_STORE_HEADER_BYTES);
    memcpy(output, key_store_magic, sizeof(key_store_magic));
    store_u16(output + 4U, KEY_STORE_FORMAT_VERSION);
    store_u16(output + 6U, KEY_STORE_HEADER_BYTES);
    store_u32(output + 8U, generation);
    store_u32(output + 12U, key_id);
    store_u32(output + 16U, key_version);
    store_u32(output + 20U, MTFS_RA8P1_AES256_WRAPPED_BYTES);
    memcpy(output + KEY_STORE_HEADER_BYTES, wrapped_key->value,
        MTFS_RA8P1_AES256_WRAPPED_BYTES);
    store_u32(output + KEY_STORE_CRC_OFFSET, record_crc(output));

    error = R_OSPI_B_Erase(&g_ospi0_ctrl,
        (uint8_t *)(uintptr_t)(KEY_STORE_BASE_ADDRESS + offsets[target]),
        MTFS_RA8P1_OSPI_KEY_SECTOR_BYTES);
    if (error != FSP_SUCCESS) {
        status = set_fsp_error(error, diagnostics);
        goto cleanup;
    }
    if (diagnostics != NULL) {
        ++diagnostics->erase_count;
    }
    status = wait_ready(diagnostics);
    if (status != MTFS_RA8P1_KEY_STORE_OK) {
        goto cleanup;
    }
    while (written < KEY_STORE_PROGRAM_BYTES) {
        uint32_t chunk = KEY_STORE_PROGRAM_BYTES - written;

        if (chunk > KEY_STORE_PROGRAM_PAGE_BYTES) {
            chunk = KEY_STORE_PROGRAM_PAGE_BYTES;
        }
        error = R_OSPI_B_Write(&g_ospi0_ctrl, output + written,
            (uint8_t *)(uintptr_t)(KEY_STORE_BASE_ADDRESS +
                offsets[target] + written), chunk);
        if (error != FSP_SUCCESS) {
            status = set_fsp_error(error, diagnostics);
            goto cleanup;
        }
        if (diagnostics != NULL) {
            ++diagnostics->write_count;
        }
        status = wait_ready(diagnostics);
        if (status != MTFS_RA8P1_KEY_STORE_OK) {
            goto cleanup;
        }
        written += chunk;
    }
    invalidate_mapped_range(slot_address(offsets[target]), sizeof(verify));
    memcpy(verify, slot_address(offsets[target]), sizeof(verify));
    if (!decode_record(verify, NULL, &verified, offsets[target]) ||
        (memcmp(output, verify, sizeof(verify)) != 0)) {
        status = MTFS_RA8P1_KEY_STORE_INVALID_RECORD;
        if (diagnostics != NULL) {
            diagnostics->last_status = status;
        }
        goto cleanup;
    }
    if (diagnostics != NULL) {
        ++diagnostics->verify_count;
        diagnostics->last_status = MTFS_RA8P1_KEY_STORE_OK;
    }
    *metadata = verified;
    status = MTFS_RA8P1_KEY_STORE_OK;

cleanup:
    mtfs_ra8p1_ospi_key_store_zero(records, sizeof(records));
    mtfs_ra8p1_ospi_key_store_zero(output, sizeof(output));
    mtfs_ra8p1_ospi_key_store_zero(verify, sizeof(verify));
    return status;
}

const char *mtfs_ra8p1_key_store_status_string(
    mtfs_ra8p1_key_store_status_t status)
{
    switch (status) {
    case MTFS_RA8P1_KEY_STORE_OK:
        return "ok";
    case MTFS_RA8P1_KEY_STORE_NOT_FOUND:
        return "not-found";
    case MTFS_RA8P1_KEY_STORE_ALREADY_PROVISIONED:
        return "already-provisioned";
    case MTFS_RA8P1_KEY_STORE_INVALID_ARGUMENT:
        return "invalid-argument";
    case MTFS_RA8P1_KEY_STORE_INVALID_RECORD:
        return "invalid-record";
    case MTFS_RA8P1_KEY_STORE_IO_ERROR:
        return "io-error";
    default:
        return "unknown";
    }
}

#else
typedef int mtfs_ra8p1_ospi_key_store_disabled_translation_unit_t;
#endif /* MTFS_ENABLE_SEALED_MODEL */
