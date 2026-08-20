#include "mtfs_stm32n6570_crypto_spike.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tm/tmonitor.h>

#include "ff.h"
#include "mtfs_stm32_saes.h"
#include "mtfs_stm32_nor_key_store.h"
#include "mtfs_stm32n6570_nor.h"

static mtfs_stm32_saes_context_t crypto_context;
static mtfs_stm32_saes_wrapped_key_t test_wrapped_key;
static uint8_t test_plaintext[MTFS_STM32_SAES_MAX_DATA_BYTES];
static uint8_t test_ciphertext[MTFS_STM32_SAES_MAX_DATA_BYTES];

#define PACKAGE_PATH             "0:/MTFSTEST.MTF"
#define PACKAGE_MANIFEST_BYTES   (200U)
#define PACKAGE_ENVELOPE_BYTES   (48U)
#define PACKAGE_CHUNK0_BYTES     (4096U)
#define PACKAGE_CHUNK1_BYTES     (904U)
#define PACKAGE_TOTAL_BYTES      (5280U)
static uint8_t package_manifest[PACKAGE_MANIFEST_BYTES];
static uint8_t package_aad[PACKAGE_MANIFEST_BYTES + 24U];
static FATFS package_filesystem;
static FIL package_file;

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p)
{
    return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4U) << 32);
}

static void write_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static int package_layout_valid(FSIZE_t bytes)
{
    static const uint8_t magic[8] = {'M','T','F','S','M','O','D',0U};
    return (bytes == PACKAGE_TOTAL_BYTES) &&
        (memcmp(package_manifest, magic, sizeof(magic)) == 0) &&
        (read_le16(package_manifest + 8U) == 1U) &&
        (read_le32(package_manifest + 12U) == 160U) &&
        (read_le32(package_manifest + 16U) == PACKAGE_MANIFEST_BYTES) &&
        (read_le32(package_manifest + 32U) == 1U) &&
        (read_le32(package_manifest + 36U) == 1U) &&
        (read_le64(package_manifest + 96U) == 5000U) &&
        (read_le64(package_manifest + 104U) == 5000U) &&
        (read_le32(package_manifest + 112U) == PACKAGE_CHUNK0_BYTES) &&
        (read_le32(package_manifest + 116U) == 2U) &&
        (read_le32(package_manifest + 120U) == 40U) &&
        (read_le32(package_manifest + 124U) == PACKAGE_ENVELOPE_BYTES);
}

static size_t envelope_aad(void)
{
    static const uint8_t domain[] = "MTFS-KEY-v1";
    memcpy(package_aad, domain, sizeof(domain));
    memcpy(package_aad + sizeof(domain), package_manifest,
        PACKAGE_MANIFEST_BYTES);
    return sizeof(domain) + PACKAGE_MANIFEST_BYTES;
}

static size_t chunk_aad(uint32_t index, uint32_t bytes)
{
    static const uint8_t domain[] = "MTFS-CHUNK-v1";
    size_t suffix = sizeof(domain) + PACKAGE_MANIFEST_BYTES;
    memcpy(package_aad, domain, sizeof(domain));
    memcpy(package_aad + sizeof(domain), package_manifest,
        PACKAGE_MANIFEST_BYTES);
    write_le32(package_aad + suffix, index);
    write_le32(package_aad + suffix + 4U, bytes);
    return suffix + 8U;
}

static int file_read_exact(void *destination, UINT bytes, FRESULT *result)
{
    UINT transferred = 0U;
    *result = f_read(&package_file, destination, bytes, &transferred);
    return (*result == FR_OK) && (transferred == bytes);
}

static int known_pattern(const uint8_t *data, size_t bytes, size_t offset)
{
    size_t index;
    for (index = 0U; index < bytes; ++index) {
        if (data[index] != (uint8_t)((offset + index) * 7U + 3U)) return 0;
    }
    return 1;
}

static const uint8_t disposable_key[MTFS_STM32_SAES_AES256_KEY_BYTES] = {
    0x83, 0x12, 0x27, 0x49, 0x5e, 0x63, 0x74, 0xa1,
    0xb8, 0xc2, 0xd5, 0xe7, 0xf0, 0x0b, 0x1d, 0x2f,
    0x34, 0x46, 0x58, 0x6a, 0x7c, 0x8e, 0x90, 0xa2,
    0xb4, 0xc6, 0xd8, 0xea, 0xfc, 0x0e, 0x10, 0x22
};

static uint8_t pattern_byte(size_t index)
{
    return (uint8_t)((index * 29U + (index >> 8) + 0x51U) & 0xffU);
}

static int all_zero(const uint8_t *data, size_t bytes)
{
    uint8_t value = 0U;
    size_t index;
    for (index = 0U; index < bytes; ++index) {
        value |= data[index];
    }
    return value == 0U;
}

static int init_and_wrap(void)
{
    mtfs_stm32_saes_status_t status = mtfs_stm32_saes_init(&crypto_context);
    if (status == MTFS_STM32_SAES_OK) {
        status = mtfs_stm32_saes_wrap_key(&crypto_context, disposable_key,
            &test_wrapped_key);
    }
    if (status != MTFS_STM32_SAES_OK) {
        tm_printf((UB *)"[crypto] init/wrap FAIL status=%s hal=%u error=0x%08x\n",
            (UB *)mtfs_stm32_saes_status_string(status),
            crypto_context.last_hal_status, crypto_context.last_hal_error);
        return 1;
    }
    return 0;
}

static int consistency_case(size_t bytes, uint32_t sequence)
{
    static const uint8_t aad[] = "microT-FS STM32N657 SAES consistency";
    uint8_t nonce[MTFS_STM32_SAES_GCM_NONCE_BYTES] = {
        0x4d, 0x54, 0x46, 0x53, 0x53, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    uint8_t tag[MTFS_STM32_SAES_GCM_TAG_BYTES];
    mtfs_stm32_saes_status_t status;
    size_t index;
    nonce[8] = (uint8_t)(sequence >> 24);
    nonce[9] = (uint8_t)(sequence >> 16);
    nonce[10] = (uint8_t)(sequence >> 8);
    nonce[11] = (uint8_t)sequence;
    for (index = 0U; index < bytes; ++index) {
        test_plaintext[index] = pattern_byte(index);
    }
    status = mtfs_stm32_saes_encrypt_wrapped(&crypto_context,
        &test_wrapped_key, nonce, aad, sizeof(aad) - 1U,
        test_plaintext, bytes, test_ciphertext, tag);
    if (status == MTFS_STM32_SAES_OK) {
        memset(test_plaintext, 0, bytes);
        status = mtfs_stm32_saes_decrypt_wrapped(&crypto_context,
            &test_wrapped_key, nonce, aad, sizeof(aad) - 1U,
            test_ciphertext, bytes, tag, test_plaintext);
    }
    if (status == MTFS_STM32_SAES_OK) {
        for (index = 0U; index < bytes; ++index) {
            if (test_plaintext[index] != pattern_byte(index)) {
                status = MTFS_STM32_SAES_AUTH_FAILED;
                break;
            }
        }
    }
    mtfs_stm32_saes_zeroize(tag, sizeof(tag));
    mtfs_stm32_saes_zeroize(test_plaintext, bytes);
    mtfs_stm32_saes_zeroize(test_ciphertext, bytes);
    tm_printf((UB *)"[crypto] consistency bytes=%u %s hal=%u error=0x%08x\n",
        (uint32_t)bytes, status == MTFS_STM32_SAES_OK ? (UB *)"PASS" :
        (UB *)"FAIL", crypto_context.last_hal_status,
        crypto_context.last_hal_error);
    return status == MTFS_STM32_SAES_OK ? 0 : 1;
}

static int run_consistency(void)
{
    static const size_t sizes[] = {0U, 37U, 4096U, 16384U, 65536U};
    size_t index;
    int failed = init_and_wrap();
    for (index = 0U; (index < sizeof(sizes) / sizeof(sizes[0])) && !failed;
        ++index) {
        failed |= consistency_case(sizes[index], (uint32_t)index + 1U);
    }
    if (!failed) {
        /* Prove that the persisted blob is usable after handle reinitialization. */
        mtfs_stm32_saes_wrapped_key_t saved = test_wrapped_key;
        failed |= mtfs_stm32_saes_init(&crypto_context) != MTFS_STM32_SAES_OK;
        test_wrapped_key = saved;
        mtfs_stm32_saes_zeroize(&saved, sizeof(saved));
        failed |= consistency_case(37U, UINT32_C(0x80000001));
    }
    mtfs_stm32_saes_zeroize(&test_wrapped_key, sizeof(test_wrapped_key));
    tm_printf((UB *)"[crypto] consistency overall %s\n",
        failed ? (UB *)"FAIL" : (UB *)"PASS");
    return failed;
}

static int negative_case(int corrupt_ciphertext)
{
    static const uint8_t aad[] = "authenticated metadata";
    static const uint8_t nonce[MTFS_STM32_SAES_GCM_NONCE_BYTES] = {
        0x4d, 0x54, 0x46, 0x53, 0x4e, 0x45, 0x47, 0x00, 0x00, 0x00, 0x00, 0x01
    };
    uint8_t tag[MTFS_STM32_SAES_GCM_TAG_BYTES];
    mtfs_stm32_saes_status_t status;
    size_t index;
    for (index = 0U; index < 37U; ++index) {
        test_plaintext[index] = pattern_byte(index);
    }
    status = mtfs_stm32_saes_encrypt_wrapped(&crypto_context,
        &test_wrapped_key, nonce, aad, sizeof(aad) - 1U,
        test_plaintext, 37U, test_ciphertext, tag);
    if (status != MTFS_STM32_SAES_OK) {
        return 1;
    }
    if (corrupt_ciphertext) {
        test_ciphertext[18] ^= 0x40U;
    } else {
        tag[7] ^= 0x80U;
    }
    memset(test_plaintext, 0xa5, 37U);
    status = mtfs_stm32_saes_decrypt_wrapped(&crypto_context,
        &test_wrapped_key, nonce, aad, sizeof(aad) - 1U,
        test_ciphertext, 37U, tag, test_plaintext);
    if ((status != MTFS_STM32_SAES_AUTH_FAILED) ||
        !all_zero(test_plaintext, 37U)) {
        tm_printf((UB *)"[crypto] negative %s FAIL status=%s zero=%u\n",
            corrupt_ciphertext ? (UB *)"ciphertext" : (UB *)"tag",
            (UB *)mtfs_stm32_saes_status_string(status),
            (uint32_t)all_zero(test_plaintext, 37U));
        return 1;
    }
    tm_printf((UB *)"[crypto] negative %s PASS hal=%u error=0x%08x output=zero\n",
        corrupt_ciphertext ? (UB *)"ciphertext" : (UB *)"tag",
        crypto_context.last_hal_status, crypto_context.last_hal_error);
    return 0;
}

static int run_negative(void)
{
    int failed = init_and_wrap();
    if (!failed) {
        failed |= negative_case(0);
        failed |= negative_case(1);
    }
    mtfs_stm32_saes_zeroize(&test_wrapped_key, sizeof(test_wrapped_key));
    tm_printf((UB *)"[crypto] negative overall %s\n",
        failed ? (UB *)"FAIL" : (UB *)"PASS");
    return failed;
}

static int run_package(void)
{
    mtfs_stm32_nor_io_t io;
    mtfs_stm32_nor_wrapped_key_t stored_fleet;
    mtfs_stm32_nor_key_metadata_t metadata;
    mtfs_stm32_nor_key_store_diagnostics_t store_diag = {0};
    mtfs_stm32_saes_wrapped_key_t fleet_key, model_key;
    uint8_t raw_model_key[MTFS_STM32_SAES_AES256_KEY_BYTES];
    uint8_t nonce[MTFS_STM32_SAES_GCM_NONCE_BYTES];
    FRESULT fs_result = FR_OK;
    mtfs_stm32_saes_status_t crypto_status = MTFS_STM32_SAES_OK;
    const char *stage = "init";
    int32_t bsp_error = 0;
    int mounted = 0, opened = 0, failed = 1;
    size_t aad_bytes;

    memset(&stored_fleet, 0, sizeof(stored_fleet));
    memset(&fleet_key, 0, sizeof(fleet_key));
    memset(&model_key, 0, sizeof(model_key));
    memset(raw_model_key, 0, sizeof(raw_model_key));
    memset(&package_filesystem, 0, sizeof(package_filesystem));
    memset(&package_file, 0, sizeof(package_file));
    stage = "mount";
    fs_result = f_mount(&package_filesystem, "0:", 1U);
    if (fs_result != FR_OK) goto cleanup;
    mounted = 1;
    stage = "open";
    fs_result = f_open(&package_file, PACKAGE_PATH, FA_READ);
    if (fs_result != FR_OK) goto cleanup;
    opened = 1;
    stage = "manifest-read";
    if (!file_read_exact(package_manifest, sizeof(package_manifest),
        &fs_result)) goto cleanup;
    stage = "package-layout";
    if (!package_layout_valid(f_size(&package_file))) goto cleanup;
    stage = "nor-open";
    if (mtfs_stm32n6570_nor_open(&io, &bsp_error) != 0) goto cleanup;
    stage = "fleet-load";
    if (mtfs_stm32_nor_key_store_load(&io, &stored_fleet, &metadata,
        &store_diag) != MTFS_STM32_NOR_KEY_STORE_OK) goto cleanup;
    if ((metadata.key_id != read_le32(package_manifest + 32U)) ||
        (metadata.key_version != read_le32(package_manifest + 36U))) {
        stage = "fleet-metadata";
        goto cleanup;
    }
    memcpy(fleet_key.bytes, stored_fleet.bytes, sizeof(fleet_key.bytes));
    stage = "saes-init";
    crypto_status = mtfs_stm32_saes_init(&crypto_context);
    if (crypto_status != MTFS_STM32_SAES_OK) goto cleanup;
    stage = "envelope-read";
    if (!file_read_exact(test_ciphertext, PACKAGE_ENVELOPE_BYTES,
        &fs_result)) goto cleanup;
    memcpy(nonce, package_manifest + 128U, sizeof(nonce));
    aad_bytes = envelope_aad();
    stage = "envelope-auth";
    crypto_status = mtfs_stm32_saes_decrypt_wrapped(&crypto_context,
        &fleet_key, nonce, package_aad, aad_bytes, test_ciphertext,
        MTFS_STM32_SAES_AES256_KEY_BYTES,
        test_ciphertext + MTFS_STM32_SAES_AES256_KEY_BYTES,
        raw_model_key);
    if (crypto_status != MTFS_STM32_SAES_OK) goto cleanup;
    stage = "model-wrap";
    crypto_status = mtfs_stm32_saes_wrap_key(&crypto_context,
        raw_model_key, &model_key);
    mtfs_stm32_saes_zeroize(raw_model_key, sizeof(raw_model_key));
    if ((crypto_status != MTFS_STM32_SAES_OK) ||
        !all_zero(raw_model_key, sizeof(raw_model_key))) goto cleanup;
    tm_printf((UB *)"[crypto-package-test] envelope PASS model-wrap=PASS raw-k-model-zeroize=PASS\n");

    stage = "chunk-0-read";
    if (!file_read_exact(test_ciphertext,
        PACKAGE_CHUNK0_BYTES + MTFS_STM32_SAES_GCM_TAG_BYTES,
        &fs_result)) goto cleanup;
    memcpy(nonce, package_manifest + 140U, 8U);
    write_le32(nonce + 8U, 0U);
    aad_bytes = chunk_aad(0U, PACKAGE_CHUNK0_BYTES);
    stage = "chunk-0-auth";
    crypto_status = mtfs_stm32_saes_decrypt_wrapped(&crypto_context,
        &model_key, nonce, package_aad, aad_bytes, test_ciphertext,
        PACKAGE_CHUNK0_BYTES, test_ciphertext + PACKAGE_CHUNK0_BYTES,
        test_plaintext);
    if ((crypto_status != MTFS_STM32_SAES_OK) ||
        !known_pattern(test_plaintext, PACKAGE_CHUNK0_BYTES, 0U)) goto cleanup;
    mtfs_stm32_saes_zeroize(test_plaintext, PACKAGE_CHUNK0_BYTES);
    tm_printf((UB *)"[crypto-package-test] payload chunk=0 bytes=4096 PASS\n");

    stage = "chunk-1-read";
    if (!file_read_exact(test_ciphertext,
        PACKAGE_CHUNK1_BYTES + MTFS_STM32_SAES_GCM_TAG_BYTES,
        &fs_result)) goto cleanup;
    memcpy(nonce, package_manifest + 140U, 8U);
    write_le32(nonce + 8U, 1U);
    aad_bytes = chunk_aad(1U, PACKAGE_CHUNK1_BYTES);
    stage = "chunk-1-auth";
    crypto_status = mtfs_stm32_saes_decrypt_wrapped(&crypto_context,
        &model_key, nonce, package_aad, aad_bytes, test_ciphertext,
        PACKAGE_CHUNK1_BYTES, test_ciphertext + PACKAGE_CHUNK1_BYTES,
        test_plaintext);
    if ((crypto_status != MTFS_STM32_SAES_OK) ||
        !known_pattern(test_plaintext, PACKAGE_CHUNK1_BYTES,
            PACKAGE_CHUNK0_BYTES)) goto cleanup;
    mtfs_stm32_saes_zeroize(test_plaintext, PACKAGE_CHUNK1_BYTES);
    tm_printf((UB *)"[crypto-package-test] payload chunk=1 bytes=904 PASS\n");
    failed = 0;

cleanup:
    mtfs_stm32_saes_zeroize(raw_model_key, sizeof(raw_model_key));
    mtfs_stm32_saes_zeroize(&stored_fleet, sizeof(stored_fleet));
    mtfs_stm32_saes_zeroize(&fleet_key, sizeof(fleet_key));
    mtfs_stm32_saes_zeroize(&model_key, sizeof(model_key));
    mtfs_stm32_saes_zeroize(nonce, sizeof(nonce));
    mtfs_stm32_saes_zeroize(package_aad, sizeof(package_aad));
    mtfs_stm32_saes_zeroize(package_manifest, sizeof(package_manifest));
    mtfs_stm32_saes_zeroize(test_plaintext, sizeof(test_plaintext));
    mtfs_stm32_saes_zeroize(test_ciphertext, sizeof(test_ciphertext));
    if (opened) (void)f_close(&package_file);
    if (mounted) (void)f_mount(NULL, "0:", 0U);
    if (failed) {
        tm_printf((UB *)"[crypto-package-test] FAIL stage=%s fatfs=%d crypto=%s hal=%u error=0x%08x bsp=%d store=%s\n",
            (UB *)stage, fs_result,
            (UB *)mtfs_stm32_saes_status_string(crypto_status),
            crypto_context.last_hal_status, crypto_context.last_hal_error,
            bsp_error,
            (UB *)mtfs_stm32_nor_key_store_status_string(store_diag.last_status));
    } else {
        tm_printf((UB *)"[crypto-package-test] PASS plaintext=5000 scratch=zeroized SD/FatFs=cleanup key-version=%u\n",
            metadata.key_version);
    }
    return failed;
}

static int run_package_negative(void)
{
    mtfs_stm32_nor_io_t io;
    mtfs_stm32_nor_wrapped_key_t stored;
    mtfs_stm32_nor_key_metadata_t metadata;
    mtfs_stm32_nor_key_store_diagnostics_t store_diag = {0};
    mtfs_stm32_saes_wrapped_key_t fleet, model;
    uint8_t raw_model[MTFS_STM32_SAES_AES256_KEY_BYTES];
    uint8_t nonce[MTFS_STM32_SAES_GCM_NONCE_BYTES];
    FRESULT fs = FR_OK;
    mtfs_stm32_saes_status_t status;
    size_t aad_bytes;
    int32_t bsp_error;
    int mounted = 0, opened = 0;
    int envelope_rejected = 0, cipher_rejected = 0, tag_rejected = 0;

    memset(&stored, 0, sizeof(stored));
    memset(&fleet, 0, sizeof(fleet));
    memset(&model, 0, sizeof(model));
    memset(raw_model, 0, sizeof(raw_model));
    fs = f_mount(&package_filesystem, "0:", 1U);
    if (fs != FR_OK) goto cleanup;
    mounted = 1;
    fs = f_open(&package_file, PACKAGE_PATH, FA_READ);
    if (fs != FR_OK) goto cleanup;
    opened = 1;
    if (!file_read_exact(package_manifest, sizeof(package_manifest), &fs) ||
        !package_layout_valid(f_size(&package_file))) goto cleanup;
    if ((mtfs_stm32n6570_nor_open(&io, &bsp_error) != 0) ||
        (mtfs_stm32_nor_key_store_load(&io, &stored, &metadata,
            &store_diag) != MTFS_STM32_NOR_KEY_STORE_OK)) goto cleanup;
    memcpy(fleet.bytes, stored.bytes, sizeof(fleet.bytes));
    if (mtfs_stm32_saes_init(&crypto_context) != MTFS_STM32_SAES_OK) goto cleanup;
    if (!file_read_exact(test_ciphertext, PACKAGE_ENVELOPE_BYTES, &fs)) goto cleanup;
    memcpy(nonce, package_manifest + 128U, sizeof(nonce));
    aad_bytes = envelope_aad();
    test_ciphertext[PACKAGE_ENVELOPE_BYTES - 1U] ^= 1U;
    memset(raw_model, 0xa5, sizeof(raw_model));
    status = mtfs_stm32_saes_decrypt_wrapped(&crypto_context, &fleet,
        nonce, package_aad, aad_bytes, test_ciphertext,
        MTFS_STM32_SAES_AES256_KEY_BYTES,
        test_ciphertext + MTFS_STM32_SAES_AES256_KEY_BYTES, raw_model);
    envelope_rejected = (status == MTFS_STM32_SAES_AUTH_FAILED) &&
        all_zero(raw_model, sizeof(raw_model));
    test_ciphertext[PACKAGE_ENVELOPE_BYTES - 1U] ^= 1U;
    status = mtfs_stm32_saes_decrypt_wrapped(&crypto_context, &fleet,
        nonce, package_aad, aad_bytes, test_ciphertext,
        MTFS_STM32_SAES_AES256_KEY_BYTES,
        test_ciphertext + MTFS_STM32_SAES_AES256_KEY_BYTES, raw_model);
    if (status != MTFS_STM32_SAES_OK) goto cleanup;
    status = mtfs_stm32_saes_wrap_key(&crypto_context, raw_model, &model);
    mtfs_stm32_saes_zeroize(raw_model, sizeof(raw_model));
    if ((status != MTFS_STM32_SAES_OK) ||
        !all_zero(raw_model, sizeof(raw_model))) goto cleanup;
    if (!file_read_exact(test_ciphertext,
        PACKAGE_CHUNK0_BYTES + MTFS_STM32_SAES_GCM_TAG_BYTES, &fs)) goto cleanup;
    memcpy(nonce, package_manifest + 140U, 8U);
    write_le32(nonce + 8U, 0U);
    aad_bytes = chunk_aad(0U, PACKAGE_CHUNK0_BYTES);
    test_ciphertext[17] ^= 1U;
    memset(test_plaintext, 0xa5, PACKAGE_CHUNK0_BYTES);
    status = mtfs_stm32_saes_decrypt_wrapped(&crypto_context, &model,
        nonce, package_aad, aad_bytes, test_ciphertext, PACKAGE_CHUNK0_BYTES,
        test_ciphertext + PACKAGE_CHUNK0_BYTES, test_plaintext);
    cipher_rejected = (status == MTFS_STM32_SAES_AUTH_FAILED) &&
        all_zero(test_plaintext, PACKAGE_CHUNK0_BYTES);
    test_ciphertext[17] ^= 1U;
    test_ciphertext[PACKAGE_CHUNK0_BYTES + 5U] ^= 1U;
    memset(test_plaintext, 0xa5, PACKAGE_CHUNK0_BYTES);
    status = mtfs_stm32_saes_decrypt_wrapped(&crypto_context, &model,
        nonce, package_aad, aad_bytes, test_ciphertext, PACKAGE_CHUNK0_BYTES,
        test_ciphertext + PACKAGE_CHUNK0_BYTES, test_plaintext);
    tag_rejected = (status == MTFS_STM32_SAES_AUTH_FAILED) &&
        all_zero(test_plaintext, PACKAGE_CHUNK0_BYTES);

cleanup:
    mtfs_stm32_saes_zeroize(&stored, sizeof(stored));
    mtfs_stm32_saes_zeroize(&fleet, sizeof(fleet));
    mtfs_stm32_saes_zeroize(&model, sizeof(model));
    mtfs_stm32_saes_zeroize(raw_model, sizeof(raw_model));
    mtfs_stm32_saes_zeroize(nonce, sizeof(nonce));
    mtfs_stm32_saes_zeroize(package_aad, sizeof(package_aad));
    mtfs_stm32_saes_zeroize(package_manifest, sizeof(package_manifest));
    mtfs_stm32_saes_zeroize(test_plaintext, sizeof(test_plaintext));
    mtfs_stm32_saes_zeroize(test_ciphertext, sizeof(test_ciphertext));
    if (opened) (void)f_close(&package_file);
    if (mounted) (void)f_mount(NULL, "0:", 0U);
    tm_printf((UB *)"[crypto-negative] package envelope-tag=%s chunk-ciphertext=%s chunk-tag=%s output-zeroize=%s %s\n",
        envelope_rejected ? (UB *)"REJECT" : (UB *)"FAIL",
        cipher_rejected ? (UB *)"REJECT" : (UB *)"FAIL",
        tag_rejected ? (UB *)"REJECT" : (UB *)"FAIL",
        (envelope_rejected && cipher_rejected && tag_rejected) ?
            (UB *)"PASS" : (UB *)"FAIL",
        (envelope_rejected && cipher_rejected && tag_rejected) ?
            (UB *)"PASS" : (UB *)"FAIL");
    return !(envelope_rejected && cipher_rejected && tag_rejected);
}

static void print_info(void)
{
    mtfs_stm32_nor_io_t io;
    mtfs_stm32_nor_wrapped_key_t stored_key;
    mtfs_stm32_nor_key_metadata_t metadata;
    mtfs_stm32_nor_key_store_diagnostics_t diagnostics = {0};
    mtfs_stm32_nor_key_store_status_t store_status =
        MTFS_STM32_NOR_KEY_STORE_IO_ERROR;
    int32_t bsp_error = 0;
    tm_printf((UB *)"[crypto] provider=STM32N657 SAES key=AES-256 keysel=DHUK mode=wrapped GCM=one-shot\n");
    tm_printf((UB *)"[crypto] build=FullSecure privileged=%u CONTROL=0x%08x SAES_CR=0x%08x SAES_SR=0x%08x\n",
        (__get_CONTROL() & CONTROL_nPRIV_Msk) == 0U ? 1U : 0U,
        __get_CONTROL(), SAES->CR, SAES->SR);
    tm_printf((UB *)"[crypto] HAL-auth-policy=middleware-tag-compare publish-after-auth max-data=%u max-aad=%u\n",
        MTFS_STM32_SAES_MAX_DATA_BYTES, MTFS_STM32_SAES_MAX_AAD_BYTES);
    if (mtfs_stm32n6570_nor_open(&io, &bsp_error) == 0) {
        store_status = mtfs_stm32_nor_key_store_load(&io, &stored_key,
            &metadata, &diagnostics);
    }
    tm_printf((UB *)"[crypto] nor=MX66UW1G45G bytes=%u slots=0x%08x/0x%08x store=%s bsp=%d valid=%u\n",
        MTFS_STM32_NOR_BYTES, MTFS_STM32_NOR_KEY_OFFSET_A,
        MTFS_STM32_NOR_KEY_OFFSET_B,
        (UB *)mtfs_stm32_nor_key_store_status_string(store_status),
        bsp_error, diagnostics.valid_slots);
    if (store_status == MTFS_STM32_NOR_KEY_STORE_OK) {
        tm_printf((UB *)"[crypto] key-id=%u key-version=%u generation=%u slot=0x%08x\n",
            metadata.key_id, metadata.key_version, metadata.generation,
            metadata.slot_offset);
    }
    mtfs_stm32_saes_zeroize(&stored_key, sizeof(stored_key));
}

int mtfs_stm32n6570_crypto_command(const char *line)
{
    if (strcmp(line, "crypto-info") == 0) {
        print_info();
        return 1;
    }
    if (strcmp(line, "crypto-consistency") == 0) {
        (void)run_consistency();
        return 1;
    }
    if (strcmp(line, "crypto-negative") == 0) {
        (void)run_negative();
        (void)run_package_negative();
        return 1;
    }
    if (strcmp(line, "crypto-package-test") == 0) {
        (void)run_package();
        return 1;
    }
    return 0;
}
