#include "test_wrapped_key_record.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mtfs_wrapped_key_fatfs.h"
#include "mtfs_wrapped_key_record.h"
#include "mtfs_stm32_nor_key_store.h"
#include "app/mtfs_key_provision_command.h"

#define TEST_RECORD_BYTES MTFS_WRAPPED_KEY_RECORD_RSIP_AES256_BYTES

typedef struct mock_nor
{
    uint8_t sectors[2][MTFS_STM32_NOR_ERASE_BYTES];
    uint32_t erase_calls;
    uint32_t program_calls;
    uint32_t fail_program_call;
} mock_nor_t;

static int mock_slot(uint32_t offset, size_t bytes, size_t *within)
{
    if ((offset >= MTFS_STM32_NOR_KEY_OFFSET_A) &&
        ((uint64_t)offset + bytes <=
            (uint64_t)MTFS_STM32_NOR_KEY_OFFSET_A +
                MTFS_STM32_NOR_ERASE_BYTES)) {
        *within = offset - MTFS_STM32_NOR_KEY_OFFSET_A;
        return 0;
    }
    if ((offset >= MTFS_STM32_NOR_KEY_OFFSET_B) &&
        ((uint64_t)offset + bytes <=
            (uint64_t)MTFS_STM32_NOR_KEY_OFFSET_B +
                MTFS_STM32_NOR_ERASE_BYTES)) {
        *within = offset - MTFS_STM32_NOR_KEY_OFFSET_B;
        return 1;
    }
    return -1;
}

static int mock_nor_read(void *opaque, uint32_t offset, uint8_t *data,
    size_t bytes)
{
    mock_nor_t *nor = (mock_nor_t *)opaque;
    size_t within;
    int slot = mock_slot(offset, bytes, &within);
    if (slot < 0) return -10;
    memcpy(data, nor->sectors[slot] + within, bytes);
    return 0;
}

static int mock_nor_erase(void *opaque, uint32_t offset)
{
    mock_nor_t *nor = (mock_nor_t *)opaque;
    size_t within;
    int slot = mock_slot(offset, MTFS_STM32_NOR_ERASE_BYTES, &within);
    if ((slot < 0) || (within != 0U)) return -11;
    ++nor->erase_calls;
    memset(nor->sectors[slot], 0xff, MTFS_STM32_NOR_ERASE_BYTES);
    return 0;
}

static void test_store_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t test_store_crc(const uint8_t *record)
{
    uint32_t crc = UINT32_MAX;
    size_t index;
    for (index = 0U; index < MTFS_STM32_NOR_KEY_RECORD_BYTES; ++index) {
        uint8_t value = ((index >= 24U) && (index < 28U)) ? 0U : record[index];
        uint32_t bit;
        if ((index >= 28U) && (index < 32U)) continue;
        crc ^= value;
        for (bit = 0U; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return crc ^ UINT32_MAX;
}

static int mock_nor_program(void *opaque, uint32_t offset,
    const uint8_t *data, size_t bytes)
{
    mock_nor_t *nor = (mock_nor_t *)opaque;
    size_t within, index;
    int slot = mock_slot(offset, bytes, &within);
    ++nor->program_calls;
    if (nor->program_calls == nor->fail_program_call) return -12;
    if (slot < 0) return -13;
    for (index = 0U; index < bytes; ++index) {
        if ((uint8_t)(nor->sectors[slot][within + index] & data[index]) !=
            data[index]) return -14;
        nor->sectors[slot][within + index] &= data[index];
    }
    return 0;
}

static int test_stm32_nor_store(mtfs_test_t *test)
{
    mock_nor_t nor;
    mtfs_stm32_nor_io_t io = {
        &nor, mock_nor_read, mock_nor_erase, mock_nor_program
    };
    mtfs_stm32_nor_wrapped_key_t key1, key2, loaded;
    mtfs_stm32_nor_key_metadata_t metadata;
    mtfs_stm32_nor_key_store_diagnostics_t diagnostics = {0};
    size_t index;
    if (!MTFS_TEST_CHECK(test,
            MTFS_STM32_NOR_KEY_OFFSET_A == UINT32_C(0x07ffe000) &&
                MTFS_STM32_NOR_KEY_OFFSET_B == UINT32_C(0x07fff000) &&
                MTFS_STM32_NOR_KEY_OFFSET_B +
                    MTFS_STM32_NOR_ERASE_BYTES == MTFS_STM32_NOR_BYTES,
            "reserve the final two 4 KiB STM32 NOR sectors for keys")) return 1;
    memset(&nor, 0xff, sizeof(nor));
    nor.erase_calls = 0U;
    nor.program_calls = 0U;
    nor.fail_program_call = 0U;
    for (index = 0U; index < sizeof(key1.bytes); ++index) {
        key1.bytes[index] = (uint8_t)(index * 3U + 1U);
        key2.bytes[index] = (uint8_t)(index * 5U + 7U);
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_nor_key_store_load(&io, &loaded, &metadata,
                &diagnostics) == MTFS_STM32_NOR_KEY_STORE_NOT_FOUND,
            "empty STM32 NOR key slots are not provisioned")) return 1;
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_nor_key_store_commit(&io, &key1, 1U, 1U, 0,
                &metadata, &diagnostics) == MTFS_STM32_NOR_KEY_STORE_OK &&
                metadata.generation == 1U &&
                metadata.slot_offset == MTFS_STM32_NOR_KEY_OFFSET_A &&
                diagnostics.write_count == 2U,
            "STM32 NOR commit verifies staged data then writes commit marker")) return 1;
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_nor_key_store_load(&io, &loaded, &metadata,
                &diagnostics) == MTFS_STM32_NOR_KEY_STORE_OK &&
                memcmp(loaded.bytes, key1.bytes, sizeof(key1.bytes)) == 0,
            "load committed STM32 DHUK-wrapped key")) return 1;
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_nor_key_store_commit(&io, &key2, 1U, 1U, 0,
                &metadata, &diagnostics) ==
                    MTFS_STM32_NOR_KEY_STORE_ALREADY_PROVISIONED,
            "reject STM32 fleet-key reprovision by default")) return 1;
    nor.fail_program_call = nor.program_calls + 2U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_nor_key_store_commit(&io, &key2, 1U, 1U, 1,
                &metadata, &diagnostics) == MTFS_STM32_NOR_KEY_STORE_IO_ERROR,
            "a power-loss-like commit write failure is rejected")) return 1;
    nor.fail_program_call = 0U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_nor_key_store_load(&io, &loaded, &metadata,
                &diagnostics) == MTFS_STM32_NOR_KEY_STORE_OK &&
                metadata.generation == 1U &&
                memcmp(loaded.bytes, key1.bytes, sizeof(key1.bytes)) == 0,
            "an uncommitted inactive slot cannot replace the old key")) return 1;
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_nor_key_store_commit(&io, &key2, 1U, 1U, 1,
                &metadata, &diagnostics) == MTFS_STM32_NOR_KEY_STORE_OK &&
                metadata.generation == 2U &&
                metadata.key_id == 1U && metadata.key_version == 1U &&
                metadata.slot_offset == MTFS_STM32_NOR_KEY_OFFSET_B,
            "replace in the inactive slot while preserving key ID/version")) return 1;
    {
        uint32_t erase_calls = nor.erase_calls;
        uint32_t program_calls = nor.program_calls;
        test_store_u32(nor.sectors[1] + 8U, UINT32_MAX);
        test_store_u32(nor.sectors[1] + 24U,
            test_store_crc(nor.sectors[1]));
        memset(&diagnostics, 0, sizeof(diagnostics));
        if (!MTFS_TEST_CHECK(test,
                mtfs_stm32_nor_key_store_commit(&io, &key1, 1U, 1U, 1,
                    &metadata, &diagnostics) ==
                        MTFS_STM32_NOR_KEY_STORE_GENERATION_EXHAUSTED &&
                    nor.erase_calls == erase_calls &&
                    nor.program_calls == program_calls &&
                    diagnostics.erase_count == 0U &&
                    diagnostics.write_count == 0U,
                "generation exhaustion is rejected before erase/program")) return 1;
        test_store_u32(nor.sectors[1] + 8U, 2U);
        test_store_u32(nor.sectors[1] + 24U,
            test_store_crc(nor.sectors[1]));
    }
    nor.sectors[1][40] ^= 1U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_stm32_nor_key_store_load(&io, &loaded, &metadata,
                &diagnostics) == MTFS_STM32_NOR_KEY_STORE_OK &&
                metadata.generation == 1U && diagnostics.valid_slots == 1U,
            "fall back to the older valid slot after record corruption")) return 1;
    return 0;
}

static void fill_blob(uint8_t *blob)
{
    size_t index;
    for (index = 0U;
         index < MTFS_WRAPPED_KEY_RECORD_RSIP_AES256_BLOB_BYTES;
         ++index) {
        blob[index] = (uint8_t)(index * 13U + 7U);
    }
}

int test_wrapped_key_record(mtfs_test_t *test)
{
    static const char crc_vector[] = "123456789";
    mtfs_wrapped_key_metadata_t input = {
        MTFS_WRAPPED_KEY_PROVIDER_RA_RSIP_E50D,
        MTFS_WRAPPED_KEY_TYPE_AES_256,
        1U,
        7U
    };
    mtfs_wrapped_key_metadata_t output;
    uint8_t blob[MTFS_WRAPPED_KEY_RECORD_RSIP_AES256_BLOB_BYTES];
    uint8_t record[TEST_RECORD_BYTES];
    uint8_t mutation[TEST_RECORD_BYTES + 1U];
    const uint8_t *decoded_blob = NULL;
    size_t encoded_bytes = 0U;
    mtfs_wrapped_key_record_status_t status;

    if (!MTFS_TEST_CHECK(test,
            mtfs_key_provision_command_parse("provision-xmodem") ==
                MTFS_KEY_PROVISION_COMMAND_INITIAL &&
            mtfs_key_provision_command_parse("provision-xmodem replace") ==
                MTFS_KEY_PROVISION_COMMAND_REPLACE &&
            mtfs_key_provision_command_parse("update-xmodem") ==
                MTFS_KEY_PROVISION_COMMAND_LEGACY_UPDATE &&
            strstr(mtfs_key_provision_legacy_guidance(),
                "versioned key rotation is unsupported") != NULL,
            "parse initial/replacement commands and keep legacy update guidance-only")) {
        return 1;
    }

    fill_blob(blob);
    if (!MTFS_TEST_CHECK(test,
            mtfs_wrapped_key_record_crc32(crc_vector,
                sizeof(crc_vector) - 1U) == UINT32_C(0xcbf43926),
            "wrapped-key CRC32 matches the standard check value")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_wrapped_key_record_size(sizeof(blob)) ==
                TEST_RECORD_BYTES,
            "RSIP AES-256 record has the fixed 84-byte size")) {
        return 1;
    }
    status = mtfs_wrapped_key_record_encode(record, sizeof(record),
        &input, blob, sizeof(blob), &encoded_bytes);
    if (!MTFS_TEST_CHECK(test,
            status == MTFS_WRAPPED_KEY_RECORD_OK &&
                encoded_bytes == sizeof(record),
            "encode a complete wrapped-key record")) {
        return 1;
    }
    status = mtfs_wrapped_key_record_decode(record, sizeof(record),
        MTFS_WRAPPED_KEY_PROVIDER_RA_RSIP_E50D,
        MTFS_WRAPPED_KEY_TYPE_AES_256, sizeof(blob),
        &output, &decoded_blob);
    if (!MTFS_TEST_CHECK(test,
            status == MTFS_WRAPPED_KEY_RECORD_OK &&
                output.provider == input.provider &&
                output.key_type == input.key_type &&
                output.key_id == input.key_id &&
                output.key_version == input.key_version &&
                decoded_blob == record +
                    MTFS_WRAPPED_KEY_RECORD_HEADER_BYTES &&
                memcmp(decoded_blob, blob, sizeof(blob)) == 0,
            "decode metadata and wrapped blob without copying")) {
        return 1;
    }

    memcpy(mutation, record, sizeof(record));
    mutation[0] ^= 1U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_wrapped_key_record_decode(mutation, sizeof(record),
                input.provider, input.key_type, sizeof(blob),
                &output, &decoded_blob) ==
                    MTFS_WRAPPED_KEY_RECORD_INVALID_FORMAT,
            "reject an invalid magic value")) {
        return 1;
    }
    memcpy(mutation, record, sizeof(record));
    mutation[12] ^= 1U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_wrapped_key_record_decode(mutation, sizeof(record),
                input.provider, input.key_type, sizeof(blob),
                &output, &decoded_blob) ==
                    MTFS_WRAPPED_KEY_RECORD_UNSUPPORTED,
            "reject a provider mismatch before RSIP use")) {
        return 1;
    }
    memcpy(mutation, record, sizeof(record));
    mutation[32] ^= 1U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_wrapped_key_record_decode(mutation, sizeof(record),
                input.provider, input.key_type, sizeof(blob),
                &output, &decoded_blob) ==
                    MTFS_WRAPPED_KEY_RECORD_CRC_MISMATCH,
            "reject wrapped-blob corruption")) {
        return 1;
    }
    memcpy(mutation, record, sizeof(record));
    mutation[24] ^= 1U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_wrapped_key_record_decode(mutation, sizeof(record),
                input.provider, input.key_type, sizeof(blob),
                &output, &decoded_blob) ==
                    MTFS_WRAPPED_KEY_RECORD_CRC_MISMATCH,
            "reject public metadata corruption")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_wrapped_key_record_decode(record, sizeof(record) - 1U,
                input.provider, input.key_type, sizeof(blob),
                &output, &decoded_blob) ==
                    MTFS_WRAPPED_KEY_RECORD_INVALID_FORMAT,
            "reject a truncated record")) {
        return 1;
    }
    memcpy(mutation, record, sizeof(record));
    mutation[sizeof(record)] = 0U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_wrapped_key_record_decode(mutation, sizeof(mutation),
                input.provider, input.key_type, sizeof(blob),
                &output, &decoded_blob) ==
                    MTFS_WRAPPED_KEY_RECORD_INVALID_FORMAT,
            "reject trailing data")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_wrapped_key_record_encode(record, sizeof(record) - 1U,
                &input, blob, sizeof(blob), &encoded_bytes) ==
                    MTFS_WRAPPED_KEY_RECORD_BUFFER_TOO_SMALL,
            "reject an undersized encode destination")) {
        return 1;
    }
    input.key_id = 0U;
    if (!MTFS_TEST_CHECK(test,
            mtfs_wrapped_key_record_encode(record, sizeof(record),
                &input, blob, sizeof(blob), &encoded_bytes) ==
                    MTFS_WRAPPED_KEY_RECORD_INVALID_ARGUMENT,
            "reject an unassigned key identifier")) {
        return 1;
    }

    {
        mtfs_wrapped_key_metadata_t saes_input = {
            MTFS_WRAPPED_KEY_PROVIDER_STM32_SAES_DHUK,
            MTFS_WRAPPED_KEY_TYPE_AES_256,
            1U,
            1U
        };
        uint8_t saes_blob[MTFS_WRAPPED_KEY_RECORD_SAES_AES256_BLOB_BYTES];
        uint8_t saes_record[MTFS_WRAPPED_KEY_RECORD_SAES_AES256_BYTES];
        size_t index;

        for (index = 0U; index < sizeof(saes_blob); ++index) {
            saes_blob[index] = (uint8_t)(0xa5U ^ index);
        }
        status = mtfs_wrapped_key_record_encode(saes_record,
            sizeof(saes_record), &saes_input, saes_blob,
            sizeof(saes_blob), &encoded_bytes);
        if (!MTFS_TEST_CHECK(test,
                status == MTFS_WRAPPED_KEY_RECORD_OK &&
                    encoded_bytes == sizeof(saes_record),
                "encode a fixed 64-byte STM32 SAES/DHUK record")) {
            return 1;
        }
        status = mtfs_wrapped_key_record_decode(saes_record,
            sizeof(saes_record),
            MTFS_WRAPPED_KEY_PROVIDER_STM32_SAES_DHUK,
            MTFS_WRAPPED_KEY_TYPE_AES_256, sizeof(saes_blob),
            &output, &decoded_blob);
        if (!MTFS_TEST_CHECK(test,
                status == MTFS_WRAPPED_KEY_RECORD_OK &&
                    memcmp(decoded_blob, saes_blob, sizeof(saes_blob)) == 0,
                "decode the 32-byte SAES wrapped-key blob")) {
            return 1;
        }
        if (!MTFS_TEST_CHECK(test,
                mtfs_wrapped_key_record_decode(saes_record,
                    sizeof(saes_record),
                    MTFS_WRAPPED_KEY_PROVIDER_RA_RSIP_E50D,
                    MTFS_WRAPPED_KEY_TYPE_AES_256, sizeof(saes_blob),
                    &output, &decoded_blob) ==
                        MTFS_WRAPPED_KEY_RECORD_UNSUPPORTED,
                "reject an SAES record before selecting the wrong provider")) {
            return 1;
        }
        if (!MTFS_TEST_CHECK(test,
                mtfs_wrapped_key_record_decode(saes_record,
                    sizeof(saes_record),
                    MTFS_WRAPPED_KEY_PROVIDER_STM32_SAES_DHUK,
                    MTFS_WRAPPED_KEY_TYPE_AES_256,
                    sizeof(saes_blob) - 1U, &output, &decoded_blob) ==
                        MTFS_WRAPPED_KEY_RECORD_INVALID_FORMAT,
                "reject a non-32-byte SAES wrapped-key expectation")) {
            return 1;
        }
    }
    if (test_stm32_nor_store(test) != 0) {
        return 1;
    }
    return 0;
}

int test_wrapped_key_fatfs(mtfs_test_t *test, const char *volume)
{
    mtfs_wrapped_key_metadata_t input = {
        MTFS_WRAPPED_KEY_PROVIDER_RA_RSIP_E50D,
        MTFS_WRAPPED_KEY_TYPE_AES_256,
        1U,
        1U
    };
    mtfs_wrapped_key_metadata_t output;
    mtfs_wrapped_key_fatfs_diagnostics_t diagnostics = {0};
    FATFS filesystem;
    uint8_t blob[MTFS_WRAPPED_KEY_RECORD_RSIP_AES256_BLOB_BYTES];
    uint8_t record[TEST_RECORD_BYTES] __attribute__((aligned(16)));
    const uint8_t *decoded_blob = NULL;
    char path[32];
    char temporary_path[32];
    mtfs_wrapped_key_fatfs_status_t status;
    int mounted = 0;

    fill_blob(blob);
    (void)snprintf(path, sizeof(path), "%s/WKEYTEST.BIN", volume);
    (void)snprintf(temporary_path, sizeof(temporary_path),
        "%s/WKEYTEST.TMP", volume);
    if (!MTFS_TEST_CHECK(test,
            f_mount(&filesystem, volume, 1U) == FR_OK,
            "mount the temporary host volume for wrapped-key I/O")) {
        goto cleanup;
    }
    mounted = 1;
    (void)f_unlink(path);
    (void)f_unlink(temporary_path);

    status = mtfs_wrapped_key_fatfs_create(path, temporary_path,
        &input, blob, sizeof(blob), record, sizeof(record), &diagnostics);
    if (!MTFS_TEST_CHECK(test,
            status == MTFS_WRAPPED_KEY_FATFS_OK &&
                diagnostics.create_count == 1U &&
                diagnostics.sync_count == 1U &&
                diagnostics.verify_count == 1U,
            "create, sync, and read back a new wrapped-key file")) {
        goto cleanup;
    }
    status = mtfs_wrapped_key_fatfs_create(path, temporary_path,
        &input, blob, sizeof(blob), record, sizeof(record), &diagnostics);
    if (!MTFS_TEST_CHECK(test,
            status == MTFS_WRAPPED_KEY_FATFS_ALREADY_EXISTS,
            "never overwrite an existing wrapped-key file")) {
        goto cleanup;
    }
    status = mtfs_wrapped_key_fatfs_load(path, record, sizeof(record),
        input.provider, input.key_type, sizeof(blob), &output,
        &decoded_blob, &diagnostics);
    if (!MTFS_TEST_CHECK(test,
            status == MTFS_WRAPPED_KEY_FATFS_OK &&
                output.key_id == input.key_id &&
                output.key_version == input.key_version &&
                memcmp(decoded_blob, blob, sizeof(blob)) == 0,
            "load the committed wrapped-key file")) {
        goto cleanup;
    }
    if (!MTFS_TEST_CHECK(test,
            f_unlink(path) == FR_OK,
            "remove only the host-test wrapped-key file")) {
        goto cleanup;
    }
    {
        FIL temporary;
        UINT written = 0U;
        uint8_t marker = 0xa5U;
        FRESULT result = f_open(&temporary, temporary_path,
            FA_WRITE | FA_CREATE_NEW);
        if (result == FR_OK) {
            result = f_write(&temporary, &marker, 1U, &written);
            (void)f_close(&temporary);
        }
        if (!MTFS_TEST_CHECK(test,
                result == FR_OK && written == 1U,
                "create a stale temporary-file sentinel")) {
            goto cleanup;
        }
    }
    status = mtfs_wrapped_key_fatfs_create(path, temporary_path,
        &input, blob, sizeof(blob), record, sizeof(record), &diagnostics);
    if (!MTFS_TEST_CHECK(test,
            status == MTFS_WRAPPED_KEY_FATFS_ALREADY_EXISTS,
            "preserve a stale temporary file for explicit recovery")) {
        goto cleanup;
    }

cleanup:
    (void)f_unlink(path);
    (void)f_unlink(temporary_path);
    if (mounted) {
        (void)MTFS_TEST_CHECK(test,
            f_mount(NULL, volume, 0U) == FR_OK,
            "unmount the wrapped-key host-test volume");
    }
    return test->failures == 0U ? 0 : 1;
}
