#include "../../../mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL

#include "mtfs_stm32_nor_key_store.h"

#include <string.h>

#define STORE_FORMAT_VERSION (UINT16_C(1))
#define STORE_HEADER_BYTES   (UINT16_C(32))
#define STORE_CRC_OFFSET     (24U)
#define STORE_COMMIT_OFFSET  (28U)
#define STORE_COMMIT_VALUE   (UINT32_C(0x434d5446)) /* "FTMC", little endian. */

typedef char store_record_layout_check[
    MTFS_STM32_NOR_KEY_RECORD_BYTES ==
        STORE_HEADER_BYTES + MTFS_STM32_NOR_WRAPPED_KEY_BYTES ? 1 : -1];
typedef char store_slot_a_alignment_check[
    (MTFS_STM32_NOR_KEY_OFFSET_A % MTFS_STM32_NOR_ERASE_BYTES) == 0U ? 1 : -1];
typedef char store_slot_b_alignment_check[
    (MTFS_STM32_NOR_KEY_OFFSET_B % MTFS_STM32_NOR_ERASE_BYTES) == 0U ? 1 : -1];
typedef char store_slots_are_contiguous_check[
    MTFS_STM32_NOR_KEY_OFFSET_A + MTFS_STM32_NOR_ERASE_BYTES ==
        MTFS_STM32_NOR_KEY_OFFSET_B ? 1 : -1];
typedef char store_slots_end_at_nor_boundary_check[
    MTFS_STM32_NOR_KEY_OFFSET_B + MTFS_STM32_NOR_ERASE_BYTES ==
        MTFS_STM32_NOR_BYTES ? 1 : -1];

static const uint8_t store_magic[4] = {'M', 'T', 'F', 'K'};

static void secure_zero(void *memory, size_t bytes)
{
    volatile uint8_t *cursor = (volatile uint8_t *)memory;
    while (bytes-- != 0U) *cursor++ = 0U;
}

static uint16_t load_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t load_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void store_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t bytes)
{
    while (bytes-- != 0U) {
        uint32_t bit;
        crc ^= *data++;
        for (bit = 0U; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return crc;
}

static uint32_t record_crc(const uint8_t *record)
{
    static const uint8_t zero_crc[4] = {0U, 0U, 0U, 0U};
    uint32_t crc = UINT32_MAX;
    crc = crc_update(crc, record, STORE_CRC_OFFSET);
    crc = crc_update(crc, zero_crc, sizeof(zero_crc));
    crc = crc_update(crc, record + STORE_HEADER_BYTES,
        MTFS_STM32_NOR_WRAPPED_KEY_BYTES);
    return crc ^ UINT32_MAX;
}

static int decode(const uint8_t *record, uint32_t offset,
    mtfs_stm32_nor_wrapped_key_t *key,
    mtfs_stm32_nor_key_metadata_t *metadata)
{
    if ((memcmp(record, store_magic, sizeof(store_magic)) != 0) ||
        (load_u16(record + 4U) != STORE_FORMAT_VERSION) ||
        (load_u16(record + 6U) != STORE_HEADER_BYTES) ||
        (load_u32(record + 20U) != MTFS_STM32_NOR_WRAPPED_KEY_BYTES) ||
        (load_u32(record + STORE_COMMIT_OFFSET) != STORE_COMMIT_VALUE) ||
        (load_u32(record + STORE_CRC_OFFSET) != record_crc(record)) ||
        (load_u32(record + 8U) == 0U) ||
        (load_u32(record + 12U) == 0U)) {
        return 0;
    }
    if (key != NULL) {
        memcpy(key->bytes, record + STORE_HEADER_BYTES,
            MTFS_STM32_NOR_WRAPPED_KEY_BYTES);
    }
    if (metadata != NULL) {
        metadata->generation = load_u32(record + 8U);
        metadata->key_id = load_u32(record + 12U);
        metadata->key_version = load_u32(record + 16U);
        metadata->slot_offset = offset;
    }
    return 1;
}

static mtfs_stm32_nor_key_store_status_t io_error(int error,
    mtfs_stm32_nor_key_store_diagnostics_t *diagnostics)
{
    if (diagnostics != NULL) {
        diagnostics->last_io_error = error;
        diagnostics->last_status = MTFS_STM32_NOR_KEY_STORE_IO_ERROR;
    }
    return MTFS_STM32_NOR_KEY_STORE_IO_ERROR;
}

static mtfs_stm32_nor_key_store_status_t scan(
    const mtfs_stm32_nor_io_t *io,
    uint8_t records[2][MTFS_STM32_NOR_KEY_RECORD_BYTES], uint8_t valid[2],
    mtfs_stm32_nor_key_metadata_t metadata[2], uint32_t *valid_count,
    mtfs_stm32_nor_key_store_diagnostics_t *diagnostics)
{
    static const uint32_t offsets[2] = {
        MTFS_STM32_NOR_KEY_OFFSET_A, MTFS_STM32_NOR_KEY_OFFSET_B
    };
    uint32_t index;
    *valid_count = 0U;
    for (index = 0U; index < 2U; ++index) {
        int error = io->read(io->context, offsets[index], records[index],
            MTFS_STM32_NOR_KEY_RECORD_BYTES);
        if (error != 0) {
            return io_error(error, diagnostics);
        }
        valid[index] = (uint8_t)decode(records[index], offsets[index], NULL,
            &metadata[index]);
        *valid_count += valid[index];
    }
    return MTFS_STM32_NOR_KEY_STORE_OK;
}

static int newest(const uint8_t valid[2],
    const mtfs_stm32_nor_key_metadata_t metadata[2])
{
    if ((valid[0] == 0U) && (valid[1] == 0U)) return -1;
    if (valid[0] == 0U) return 1;
    if (valid[1] == 0U) return 0;
    return metadata[1].generation > metadata[0].generation ? 1 : 0;
}

static int valid_io(const mtfs_stm32_nor_io_t *io)
{
    return (io != NULL) && (io->read != NULL) &&
        (io->erase_sector != NULL) && (io->program != NULL);
}

mtfs_stm32_nor_key_store_status_t mtfs_stm32_nor_key_store_load(
    const mtfs_stm32_nor_io_t *io,
    mtfs_stm32_nor_wrapped_key_t *wrapped_key,
    mtfs_stm32_nor_key_metadata_t *metadata,
    mtfs_stm32_nor_key_store_diagnostics_t *diagnostics)
{
    uint8_t records[2][MTFS_STM32_NOR_KEY_RECORD_BYTES];
    uint8_t valid[2] = {0U, 0U};
    mtfs_stm32_nor_key_metadata_t found[2] = {{0U}};
    uint32_t count;
    int selected;
    mtfs_stm32_nor_key_store_status_t status;
    if (!valid_io(io) || (wrapped_key == NULL) || (metadata == NULL)) {
        return MTFS_STM32_NOR_KEY_STORE_INVALID_ARGUMENT;
    }
    status = scan(io, records, valid, found, &count, diagnostics);
    if (status != MTFS_STM32_NOR_KEY_STORE_OK) goto cleanup;
    if (diagnostics != NULL) diagnostics->valid_slots = count;
    selected = newest(valid, found);
    if (selected < 0) {
        status = MTFS_STM32_NOR_KEY_STORE_NOT_FOUND;
        goto cleanup;
    }
    (void)decode(records[selected], found[selected].slot_offset, wrapped_key,
        metadata);
    status = MTFS_STM32_NOR_KEY_STORE_OK;
cleanup:
    secure_zero(records, sizeof(records));
    if (diagnostics != NULL) diagnostics->last_status = status;
    return status;
}

mtfs_stm32_nor_key_store_status_t mtfs_stm32_nor_key_store_commit(
    const mtfs_stm32_nor_io_t *io,
    const mtfs_stm32_nor_wrapped_key_t *wrapped_key,
    uint32_t key_id, uint32_t key_version, int allow_update,
    mtfs_stm32_nor_key_metadata_t *metadata,
    mtfs_stm32_nor_key_store_diagnostics_t *diagnostics)
{
    static const uint32_t offsets[2] = {
        MTFS_STM32_NOR_KEY_OFFSET_A, MTFS_STM32_NOR_KEY_OFFSET_B
    };
    uint8_t records[2][MTFS_STM32_NOR_KEY_RECORD_BYTES];
    uint8_t valid[2] = {0U, 0U};
    uint8_t output[MTFS_STM32_NOR_KEY_RECORD_BYTES];
    uint8_t verify[MTFS_STM32_NOR_KEY_RECORD_BYTES];
    uint8_t commit_bytes[4];
    mtfs_stm32_nor_key_metadata_t found[2] = {{0U}}, verified = {0U};
    uint32_t count, generation = 1U;
    int current, target, error;
    mtfs_stm32_nor_key_store_status_t status;
    if (!valid_io(io) || (wrapped_key == NULL) || (metadata == NULL) ||
        (key_id == 0U) || (key_version == 0U)) {
        return MTFS_STM32_NOR_KEY_STORE_INVALID_ARGUMENT;
    }
    status = scan(io, records, valid, found, &count, diagnostics);
    if (status != MTFS_STM32_NOR_KEY_STORE_OK) goto cleanup;
    if (diagnostics != NULL) diagnostics->valid_slots = count;
    current = newest(valid, found);
    if ((current >= 0) && !allow_update) {
        status = MTFS_STM32_NOR_KEY_STORE_ALREADY_PROVISIONED;
        goto cleanup;
    }
    if (current >= 0) {
        if ((key_id != found[current].key_id) ||
            (key_version != found[current].key_version)) {
            status = MTFS_STM32_NOR_KEY_STORE_INVALID_ARGUMENT;
            goto cleanup;
        }
        if (found[current].generation == UINT32_MAX) {
            status = MTFS_STM32_NOR_KEY_STORE_GENERATION_EXHAUSTED;
            goto cleanup;
        }
        generation = found[current].generation + 1U;
        target = current ^ 1;
    } else {
        target = 0;
    }
    memset(output, 0, sizeof(output));
    memcpy(output, store_magic, sizeof(store_magic));
    store_u16(output + 4U, STORE_FORMAT_VERSION);
    store_u16(output + 6U, STORE_HEADER_BYTES);
    store_u32(output + 8U, generation);
    store_u32(output + 12U, key_id);
    store_u32(output + 16U, key_version);
    store_u32(output + 20U, MTFS_STM32_NOR_WRAPPED_KEY_BYTES);
    store_u32(output + STORE_COMMIT_OFFSET, UINT32_MAX);
    memcpy(output + STORE_HEADER_BYTES, wrapped_key->bytes,
        MTFS_STM32_NOR_WRAPPED_KEY_BYTES);
    store_u32(output + STORE_CRC_OFFSET, record_crc(output));
    error = io->erase_sector(io->context, offsets[target]);
    if (error != 0) { status = io_error(error, diagnostics); goto cleanup; }
    if (diagnostics != NULL) ++diagnostics->erase_count;
    error = io->program(io->context, offsets[target], output, sizeof(output));
    if (error != 0) { status = io_error(error, diagnostics); goto cleanup; }
    if (diagnostics != NULL) ++diagnostics->write_count;
    error = io->read(io->context, offsets[target], verify, sizeof(verify));
    if (error != 0) { status = io_error(error, diagnostics); goto cleanup; }
    if (memcmp(output, verify, sizeof(output)) != 0) {
        status = MTFS_STM32_NOR_KEY_STORE_INVALID_RECORD;
        goto cleanup;
    }
    if (diagnostics != NULL) ++diagnostics->verify_count;
    store_u32(commit_bytes, STORE_COMMIT_VALUE);
    error = io->program(io->context, offsets[target] + STORE_COMMIT_OFFSET,
        commit_bytes, sizeof(commit_bytes));
    if (error != 0) { status = io_error(error, diagnostics); goto cleanup; }
    if (diagnostics != NULL) ++diagnostics->write_count;
    error = io->read(io->context, offsets[target], verify, sizeof(verify));
    if (error != 0) { status = io_error(error, diagnostics); goto cleanup; }
    if (!decode(verify, offsets[target], NULL, &verified)) {
        status = MTFS_STM32_NOR_KEY_STORE_INVALID_RECORD;
        goto cleanup;
    }
    if (diagnostics != NULL) ++diagnostics->verify_count;
    *metadata = verified;
    status = MTFS_STM32_NOR_KEY_STORE_OK;
cleanup:
    secure_zero(records, sizeof(records));
    secure_zero(output, sizeof(output));
    secure_zero(verify, sizeof(verify));
    secure_zero(commit_bytes, sizeof(commit_bytes));
    if (diagnostics != NULL) diagnostics->last_status = status;
    return status;
}

const char *mtfs_stm32_nor_key_store_status_string(
    mtfs_stm32_nor_key_store_status_t status)
{
    switch (status) {
    case MTFS_STM32_NOR_KEY_STORE_OK: return "ok";
    case MTFS_STM32_NOR_KEY_STORE_NOT_FOUND: return "not-found";
    case MTFS_STM32_NOR_KEY_STORE_ALREADY_PROVISIONED: return "already-provisioned";
    case MTFS_STM32_NOR_KEY_STORE_INVALID_ARGUMENT: return "invalid-argument";
    case MTFS_STM32_NOR_KEY_STORE_INVALID_RECORD: return "invalid-record";
    case MTFS_STM32_NOR_KEY_STORE_IO_ERROR: return "io-error";
    case MTFS_STM32_NOR_KEY_STORE_GENERATION_EXHAUSTED: return "generation-exhausted";
    default: return "unknown";
    }
}

#else
typedef int mtfs_stm32_nor_key_store_disabled_translation_unit_t;
#endif /* MTFS_ENABLE_SEALED_MODEL */
