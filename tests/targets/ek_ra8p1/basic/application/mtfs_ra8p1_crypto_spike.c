/* RSIP Compatibility Mode provisioned-key validation commands. */

#include "mtfs_ra8p1_crypto_spike.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tm/tmonitor.h>

#if MTFS_RA8P1_CRYPTO_SPIKE_ENABLE
#include "ff.h"
#include "hal_data.h"
#include "psa/crypto.h"
#include "mtfs_ra8p1_ospi_key_store.h"
#include "mtfs_ra8p1_crypto_work.h"

#if !defined(MBEDTLS_PSA_ASSUME_EXCLUSIVE_BUFFERS)
#error "The RA8P1 flat-build crypto tests require exclusive PSA buffers"
#endif

#define CRYPTO_ALIGN        __attribute__((aligned(16)))
#define CRYPTO_MAX_BYTES    (64U * 1024U)
#define CRYPTO_TAG_BYTES    (16U)

#define TEST_PACKAGE_PATH                   "0:/MTFSTEST.MTF"
#define TEST_PACKAGE_PREAMBLE_BYTES         (160U)
#define TEST_PACKAGE_METADATA_BYTES         (40U)
#define TEST_PACKAGE_MANIFEST_BYTES         (TEST_PACKAGE_PREAMBLE_BYTES + TEST_PACKAGE_METADATA_BYTES)
#define TEST_PACKAGE_ENVELOPE_PLAIN_BYTES   (32U)
#define TEST_PACKAGE_ENVELOPE_BYTES         (TEST_PACKAGE_ENVELOPE_PLAIN_BYTES + CRYPTO_TAG_BYTES)
#define TEST_PACKAGE_CHUNK0_PLAIN_BYTES     (4096U)
#define TEST_PACKAGE_CHUNK1_PLAIN_BYTES     (904U)
#define TEST_PACKAGE_PAYLOAD_PLAIN_BYTES    (5000U)
#define TEST_PACKAGE_CHUNK_COUNT            (2U)
#define TEST_PACKAGE_BYTES                  (TEST_PACKAGE_MANIFEST_BYTES + TEST_PACKAGE_ENVELOPE_BYTES + \
                                    TEST_PACKAGE_PAYLOAD_PLAIN_BYTES + \
                                    TEST_PACKAGE_CHUNK_COUNT * CRYPTO_TAG_BYTES)
#define TEST_PACKAGE_KEY_NONCE_OFFSET       (128U)
#define TEST_PACKAGE_PAYLOAD_PREFIX_OFFSET  (140U)
#define TEST_PACKAGE_AAD_MAX_BYTES          (14U + TEST_PACKAGE_MANIFEST_BYTES + 8U)

typedef struct crypto_diagnostics
{
    uint32_t initialize_count;
    uint32_t import_count;
    uint32_t destroy_count;
    uint32_t encrypt_count;
    uint32_t decrypt_count;
    uint32_t auth_fail_count;
    uint32_t zeroize_count;
    int32_t last_psa_status;
} crypto_diagnostics_t;

static uint8_t plaintext[CRYPTO_MAX_BYTES] CRYPTO_ALIGN;
#define ciphertext mtfs_ra8p1_test_ciphertext_work
/*
 * FSP 6.5.0's SCE GCM final primitive writes one complete final block at
 * floor(payload_bytes / 16) * 16, including when the payload is block-aligned.
 */
#define recovered mtfs_ra8p1_test_plaintext_work
static uint8_t test_package_manifest[TEST_PACKAGE_MANIFEST_BYTES] CRYPTO_ALIGN;
static uint8_t test_package_model_key[TEST_PACKAGE_ENVELOPE_PLAIN_BYTES + CRYPTO_TAG_BYTES] CRYPTO_ALIGN;
static uint8_t test_package_aad[TEST_PACKAGE_AAD_MAX_BYTES] CRYPTO_ALIGN;
static rsip_aes_wrapped_key_t test_package_wrapped_model_key CRYPTO_ALIGN;
static FATFS test_package_filesystem;
static FIL test_package_file;
static uint8_t test_package_mounted;
static uint8_t test_package_file_open;
static crypto_diagnostics_t crypto_diag;
static uint8_t crypto_ready;

static void crypto_zero(void *data, size_t bytes)
{
    volatile uint8_t *cursor = (volatile uint8_t *)data;

    while (bytes-- != 0U) {
        *cursor++ = 0U;
    }
    __asm volatile ("" : : "r" (data) : "memory");
    ++crypto_diag.zeroize_count;
}

static int authentication_rejected(psa_status_t status)
{
    /* FSP 6.5.0 collapses RSIP GCM authentication failure into -147. */
    return (status == PSA_ERROR_INVALID_SIGNATURE) ||
        (status == PSA_ERROR_HARDWARE_FAILURE);
}

static psa_status_t locked_aead_encrypt(psa_key_handle_t handle,
    psa_algorithm_t algorithm, const uint8_t *nonce, size_t nonce_size,
    const uint8_t *aad, size_t aad_size, const uint8_t *input,
    size_t input_size, uint8_t *output, size_t output_capacity,
    size_t *output_size)
{
    psa_status_t status;
    if (mtfs_ra8p1_rsip_lock(NULL) != 0)
        return PSA_ERROR_BAD_STATE;
    status = psa_aead_encrypt(handle, algorithm, nonce, nonce_size, aad,
        aad_size, input, input_size, output, output_capacity, output_size);
    mtfs_ra8p1_rsip_unlock(NULL);
    return status;
}

static psa_status_t locked_aead_decrypt(psa_key_handle_t handle,
    psa_algorithm_t algorithm, const uint8_t *nonce, size_t nonce_size,
    const uint8_t *aad, size_t aad_size, const uint8_t *input,
    size_t input_size, uint8_t *output, size_t output_capacity,
    size_t *output_size)
{
    psa_status_t status;
    if (mtfs_ra8p1_rsip_lock(NULL) != 0)
        return PSA_ERROR_BAD_STATE;
    status = psa_aead_decrypt(handle, algorithm, nonce, nonce_size, aad,
        aad_size, input, input_size, output, output_capacity, output_size);
    mtfs_ra8p1_rsip_unlock(NULL);
    return status;
}

static fsp_err_t locked_initial_key_wrap(const uint8_t *plain_key,
    rsip_aes_wrapped_key_t *wrapped_key)
{
    fsp_err_t status;
    if (mtfs_ra8p1_rsip_lock(NULL) != 0)
        return FSP_ERR_IN_USE;
    status = R_RSIP_AES256_InitialKeyWrap(RSIP_KEY_INJECTION_TYPE_PLAIN,
        NULL, NULL, plain_key, wrapped_key);
    mtfs_ra8p1_rsip_unlock(NULL);
    return status;
}

static psa_status_t crypto_initialize(void)
{
    psa_status_t status;

    if (crypto_ready != 0U) {
        return PSA_SUCCESS;
    }
    if (mtfs_ra8p1_rsip_lock(NULL) != 0) {
        return PSA_ERROR_BAD_STATE;
    }
    status = psa_crypto_init();
    mtfs_ra8p1_rsip_unlock(NULL);
    crypto_diag.last_psa_status = status;
    if (status != PSA_SUCCESS)
        return status;
    crypto_ready = 1U;
    ++crypto_diag.initialize_count;
    return PSA_SUCCESS;
}

static psa_status_t import_wrapped_key(
    const rsip_aes_wrapped_key_t *wrapped_key,
    psa_key_handle_t *handle)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_status_t status;

    psa_set_key_usage_flags(&attributes,
        PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES_WRAPPED);
    psa_set_key_bits(&attributes, 256U);
    psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_VOLATILE);
    if (mtfs_ra8p1_rsip_lock(NULL) != 0) {
        psa_reset_key_attributes(&attributes);
        return PSA_ERROR_BAD_STATE;
    }
    status = psa_import_key(&attributes,
        (const uint8_t *)wrapped_key->value,
        MTFS_RA8P1_AES256_WRAPPED_BYTES, handle);
    mtfs_ra8p1_rsip_unlock(NULL);
    psa_reset_key_attributes(&attributes);
    crypto_diag.last_psa_status = status;
    if (status == PSA_SUCCESS) {
        ++crypto_diag.import_count;
    }
    return status;
}

static void destroy_key(psa_key_handle_t *handle)
{
    if (*handle != 0U) {
        if (mtfs_ra8p1_rsip_lock(NULL) == 0) {
            crypto_diag.last_psa_status = psa_destroy_key(*handle);
            mtfs_ra8p1_rsip_unlock(NULL);
        } else {
            crypto_diag.last_psa_status = PSA_ERROR_BAD_STATE;
        }
        ++crypto_diag.destroy_count;
        *handle = 0U;
    }
}

static void fill_pattern(uint8_t *data, size_t bytes)
{
    size_t index;

    for (index = 0U; index < bytes; ++index) {
        data[index] = (uint8_t)(index * 7U + 3U);
    }
}

static int pattern_matches(const uint8_t *data, size_t bytes)
{
    size_t index;

    for (index = 0U; index < bytes; ++index) {
        if (data[index] != (uint8_t)(index * 7U + 3U)) {
            return 0;
        }
    }
    return 1;
}

static int buffer_is_zero(const uint8_t *data, size_t bytes)
{
    size_t index;

    for (index = 0U; index < bytes; ++index) {
        if (data[index] != 0U) {
            return 0;
        }
    }
    return 1;
}

static size_t decrypt_capacity(size_t payload_bytes)
{
    return (payload_bytes & ~(size_t)(CRYPTO_TAG_BYTES - 1U)) +
        CRYPTO_TAG_BYTES;
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) |
        ((uint32_t)data[3] << 24);
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] |
        ((uint16_t)data[1] << 8));
}

static uint64_t read_le64(const uint8_t *data)
{
    return (uint64_t)read_le32(data) |
        ((uint64_t)read_le32(data + 4U) << 32);
}

static void write_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static int pattern_matches_at(const uint8_t *data, size_t bytes,
    size_t payload_offset)
{
    size_t index;

    for (index = 0U; index < bytes; ++index) {
        if (data[index] !=
            (uint8_t)((payload_offset + index) * 7U + 3U)) {
            return 0;
        }
    }
    return 1;
}

static int test_package_layout_valid(FSIZE_t file_bytes)
{
    static const uint8_t magic[8] = {
        'M','T','F','S','M','O','D',0U
    };
    static const uint8_t metadata[TEST_PACKAGE_METADATA_BYTES] = {
        0x01U, 0x00U, 0x01U, 0x00U, 0x04U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x04U, 0x00U, 0x00U, 0x00U, 0x0aU, 0x00U, 0x00U, 0x00U,
        'f', 'l', 'e', 'e', 't', '-', 't', 'e', 's', 't', 0U, 0U,
        0U, 0U, 0U, 0U
    };

    return (file_bytes == (FSIZE_t)TEST_PACKAGE_BYTES) &&
        (memcmp(test_package_manifest, magic, sizeof(magic)) == 0) &&
        (read_le16(&test_package_manifest[8]) == 1U) &&
        (read_le16(&test_package_manifest[10]) == 0U) &&
        (read_le32(&test_package_manifest[12]) == TEST_PACKAGE_PREAMBLE_BYTES) &&
        (read_le32(&test_package_manifest[16]) == TEST_PACKAGE_MANIFEST_BYTES) &&
        (read_le32(&test_package_manifest[20]) == 1U) &&
        (read_le32(&test_package_manifest[24]) == 1U) &&
        (read_le16(&test_package_manifest[28]) == 1U) &&
        (read_le16(&test_package_manifest[30]) == 1U) &&
        (read_le32(&test_package_manifest[32]) == 1U) &&
        (read_le32(&test_package_manifest[36]) == 1U) &&
        (read_le32(&test_package_manifest[80]) == 0x11U) &&
        (read_le32(&test_package_manifest[84]) == 0x22U) &&
        (read_le32(&test_package_manifest[88]) == 0x33U) &&
        (read_le64(&test_package_manifest[96]) == TEST_PACKAGE_PAYLOAD_PLAIN_BYTES) &&
        (read_le64(&test_package_manifest[104]) == TEST_PACKAGE_PAYLOAD_PLAIN_BYTES) &&
        (read_le32(&test_package_manifest[112]) == TEST_PACKAGE_CHUNK0_PLAIN_BYTES) &&
        (read_le32(&test_package_manifest[116]) == TEST_PACKAGE_CHUNK_COUNT) &&
        (read_le32(&test_package_manifest[120]) == TEST_PACKAGE_METADATA_BYTES) &&
        (read_le32(&test_package_manifest[124]) == TEST_PACKAGE_ENVELOPE_BYTES) &&
        (memcmp(&test_package_manifest[TEST_PACKAGE_PREAMBLE_BYTES], metadata,
            sizeof(metadata)) == 0);
}

static size_t test_package_build_envelope_aad(void)
{
    static const uint8_t domain[] = "MTFS-KEY-v1";

    memcpy(test_package_aad, domain, sizeof(domain));
    memcpy(&test_package_aad[sizeof(domain)], test_package_manifest,
        TEST_PACKAGE_MANIFEST_BYTES);
    return sizeof(domain) + TEST_PACKAGE_MANIFEST_BYTES;
}

static size_t test_package_build_chunk_aad(uint32_t chunk_index,
    uint32_t plain_bytes)
{
    static const uint8_t domain[] = "MTFS-CHUNK-v1";
    size_t suffix = sizeof(domain) + TEST_PACKAGE_MANIFEST_BYTES;

    memcpy(test_package_aad, domain, sizeof(domain));
    memcpy(&test_package_aad[sizeof(domain)], test_package_manifest,
        TEST_PACKAGE_MANIFEST_BYTES);
    write_le32(&test_package_aad[suffix], chunk_index);
    write_le32(&test_package_aad[suffix + 4U], plain_bytes);
    return suffix + 8U;
}

static void test_package_build_chunk_nonce(uint8_t nonce[12], uint32_t chunk_index)
{
    memcpy(nonce, &test_package_manifest[TEST_PACKAGE_PAYLOAD_PREFIX_OFFSET], 8U);
    write_le32(&nonce[8], chunk_index);
}

static int test_package_read_exact(void *destination, UINT bytes, FRESULT *result)
{
    UINT transferred = 0U;

    *result = f_read(&test_package_file, destination, bytes, &transferred);
    return (*result == FR_OK) && (transferred == bytes);
}

static void test_package_close(void)
{
    if (test_package_file_open != 0U) {
        (void)f_close(&test_package_file);
        test_package_file_open = 0U;
    }
    if (test_package_mounted != 0U) {
        (void)f_mount(NULL, "0:", 0U);
        test_package_mounted = 0U;
    }
}

static int test_package_open(const char **stage, FRESULT *result)
{
    memset(&test_package_filesystem, 0, sizeof(test_package_filesystem));
    memset(&test_package_file, 0, sizeof(test_package_file));
    memset(test_package_manifest, 0, sizeof(test_package_manifest));
    test_package_file_open = 0U;
    test_package_mounted = 0U;

    *stage = "mount";
    *result = f_mount(&test_package_filesystem, "0:", 1U);
    if (*result != FR_OK) {
        return 0;
    }
    test_package_mounted = 1U;
    *stage = "open";
    *result = f_open(&test_package_file, TEST_PACKAGE_PATH, FA_READ);
    if (*result != FR_OK) {
        test_package_close();
        return 0;
    }
    test_package_file_open = 1U;
    *stage = "manifest-read";
    if (!test_package_read_exact(test_package_manifest, sizeof(test_package_manifest), result)) {
        test_package_close();
        return 0;
    }
    *stage = "package-layout";
    if (!test_package_layout_valid(f_size(&test_package_file))) {
        *result = FR_INVALID_OBJECT;
        test_package_close();
        return 0;
    }
    return 1;
}

static int test_package_seek(FSIZE_t offset, const char **stage, FRESULT *result)
{
    *stage = "package-seek";
    *result = f_lseek(&test_package_file, offset);
    return *result == FR_OK;
}

static mtfs_ra8p1_key_store_status_t load_key(
    rsip_aes_wrapped_key_t *wrapped_key,
    mtfs_ra8p1_key_metadata_t *metadata,
    mtfs_ra8p1_key_store_diagnostics_t *store_diag)
{
    memset(wrapped_key, 0, sizeof(*wrapped_key));
    memset(metadata, 0, sizeof(*metadata));
    memset(store_diag, 0, sizeof(*store_diag));
    return mtfs_ra8p1_ospi_key_store_load(wrapped_key, metadata, store_diag);
}

static int prepare_key(psa_key_handle_t *handle,
    mtfs_ra8p1_key_metadata_t *metadata,
    mtfs_ra8p1_key_store_diagnostics_t *store_diag)
{
    rsip_aes_wrapped_key_t wrapped_key;
    mtfs_ra8p1_key_store_status_t store_status;
    psa_status_t status;

    store_status = load_key(&wrapped_key, metadata, store_diag);
    if (store_status != MTFS_RA8P1_KEY_STORE_OK) {
        tm_printf((UB *)"[crypto] BLOCKED: OSPI fleet key %s; run dedicated key-provision app\n",
            (UB *)mtfs_ra8p1_key_store_status_string(store_status));
        mtfs_ra8p1_ospi_key_store_zero(&wrapped_key, sizeof(wrapped_key));
        return 0;
    }
    status = crypto_initialize();
    if (status == PSA_SUCCESS) {
        status = import_wrapped_key(&wrapped_key, handle);
    }
    mtfs_ra8p1_ospi_key_store_zero(&wrapped_key, sizeof(wrapped_key));
    if (status != PSA_SUCCESS) {
        tm_printf((UB *)"[crypto] wrapped-key import FAIL psa=%d\n", (INT)status);
        return 0;
    }
    return 1;
}

static int run_positive_suite(void)
{
    static const size_t lengths[] = {
        0U, 37U, 4U * 1024U, 16U * 1024U, 64U * 1024U
    };
    static const uint8_t aad[] = {
        'm','i','c','r','o','T','-','F','S',' ','R','A','8','P','1'
    };
    uint8_t nonce[12] CRYPTO_ALIGN = {
        0x4dU, 0x54U, 0x46U, 0x53U, 0x2dU, 0x4bU,
        0x41U, 0x54U, 0x2dU, 0x30U, 0x30U, 0x00U
    };
    mtfs_ra8p1_key_metadata_t metadata;
    mtfs_ra8p1_key_store_diagnostics_t store_diag;
    psa_key_handle_t handle = 0U;
    psa_status_t status = PSA_SUCCESS;
    size_t cipher_bytes = 0U;
    size_t plain_bytes = 0U;
    size_t case_index;
    int failed = 0;
    const char *failed_stage = "none";

    if (!prepare_key(&handle, &metadata, &store_diag)) {
        return 1;
    }
    for (case_index = 0U;
         case_index < sizeof(lengths) / sizeof(lengths[0]); ++case_index) {
        size_t bytes = lengths[case_index];
        size_t recovered_capacity = decrypt_capacity(bytes);

        nonce[11] = (uint8_t)case_index;
        fill_pattern(plaintext, bytes);
        memset(ciphertext, 0, bytes + CRYPTO_TAG_BYTES);
        cipher_bytes = 0U;
        plain_bytes = 0U;
        status = locked_aead_encrypt(handle, PSA_ALG_GCM,
            nonce, sizeof(nonce), aad, sizeof(aad), plaintext, bytes,
            ciphertext, bytes + CRYPTO_TAG_BYTES, &cipher_bytes);
        ++crypto_diag.encrypt_count;
        crypto_diag.last_psa_status = status;
        if ((status != PSA_SUCCESS) ||
            (cipher_bytes != bytes + CRYPTO_TAG_BYTES)) {
            failed_stage = "encrypt";
            failed = 1;
            break;
        }
        memset(recovered, 0xa5, recovered_capacity);
        plain_bytes = 0U;
        status = locked_aead_decrypt(handle, PSA_ALG_GCM,
            nonce, sizeof(nonce), aad, sizeof(aad), ciphertext, cipher_bytes,
            recovered, recovered_capacity, &plain_bytes);
        ++crypto_diag.decrypt_count;
        crypto_diag.last_psa_status = status;
        if ((status != PSA_SUCCESS) || (plain_bytes != bytes) ||
            !pattern_matches(recovered, bytes)) {
            failed_stage = "decrypt";
            failed = 1;
            break;
        }
    }

    if (!failed) {
        size_t bytes = 37U;
        nonce[11] = 0xa5U;
        fill_pattern(plaintext, bytes);
        status = locked_aead_encrypt(handle, PSA_ALG_GCM,
            nonce, sizeof(nonce), aad, sizeof(aad), plaintext, bytes,
            ciphertext, bytes + CRYPTO_TAG_BYTES, &cipher_bytes);
        ++crypto_diag.encrypt_count;
        destroy_key(&handle);
        if ((status != PSA_SUCCESS) ||
            !prepare_key(&handle, &metadata, &store_diag)) {
            failed_stage = status == PSA_SUCCESS ? "reimport" :
                "reimport-encrypt";
            failed = 1;
        } else {
            plain_bytes = 0U;
            status = locked_aead_decrypt(handle, PSA_ALG_GCM,
                nonce, sizeof(nonce), aad, sizeof(aad),
                ciphertext, cipher_bytes, recovered, decrypt_capacity(bytes),
                &plain_bytes);
            ++crypto_diag.decrypt_count;
            crypto_diag.last_psa_status = status;
            if ((status != PSA_SUCCESS) || (plain_bytes != bytes) ||
                !pattern_matches(recovered, bytes)) {
                failed_stage = "reimport-decrypt";
                failed = 1;
            }
        }
    }
    if (failed) {
        tm_printf((UB *)"[crypto] consistency detail case=%u bytes=%u stage=%s psa=%d cipher-bytes=%u plain-bytes=%u\n",
            (UW)case_index,
            (UW)(case_index < sizeof(lengths) / sizeof(lengths[0]) ?
                lengths[case_index] : 37U),
            (UB *)failed_stage, (INT)status, (UW)cipher_bytes,
            (UW)plain_bytes);
    }
    destroy_key(&handle);
    crypto_zero(plaintext, sizeof(plaintext));
    crypto_zero(ciphertext, sizeof(ciphertext));
    crypto_zero(recovered, sizeof(recovered));
    tm_printf((UB *)"[crypto] provisioned-key GCM consistency empty/partial/4KiB/16KiB/64KiB/reimport %s key-version=%u\n",
        failed ? (UB *)"FAIL" : (UB *)"PASS", (UW)metadata.key_version);
    return failed;
}

static int test_package_open_model_key(psa_key_handle_t fleet_handle,
    psa_key_handle_t *model_handle, psa_status_t *last_status,
    fsp_err_t *wrap_status, FRESULT *fs_result, const char **stage,
    int *raw_zeroized)
{
    uint8_t nonce[12] CRYPTO_ALIGN;
    size_t aad_bytes = test_package_build_envelope_aad();
    size_t plain_bytes = 0U;
    psa_status_t status;

    *model_handle = 0U;
    *stage = "envelope-read";
    *wrap_status = FSP_SUCCESS;
    *raw_zeroized = 0;
    if (!test_package_read_exact(ciphertext, TEST_PACKAGE_ENVELOPE_BYTES, fs_result)) {
        return 0;
    }
    *stage = "envelope-auth";
    memcpy(nonce, &test_package_manifest[TEST_PACKAGE_KEY_NONCE_OFFSET], sizeof(nonce));
    memset(test_package_model_key, 0xa5, sizeof(test_package_model_key));
    status = locked_aead_decrypt(fleet_handle, PSA_ALG_GCM,
        nonce, sizeof(nonce), test_package_aad, aad_bytes,
        ciphertext, TEST_PACKAGE_ENVELOPE_BYTES,
        test_package_model_key, sizeof(test_package_model_key), &plain_bytes);
    ++crypto_diag.decrypt_count;
    crypto_diag.last_psa_status = status;
    *last_status = status;
    if ((status != PSA_SUCCESS) ||
        (plain_bytes != TEST_PACKAGE_ENVELOPE_PLAIN_BYTES)) {
        if (status == PSA_SUCCESS) {
            *stage = "envelope-length";
        }
        crypto_zero(test_package_model_key, sizeof(test_package_model_key));
        *raw_zeroized = buffer_is_zero(test_package_model_key,
            sizeof(test_package_model_key));
        return 0;
    }

    *stage = "model-wrap";
    memset(&test_package_wrapped_model_key, 0, sizeof(test_package_wrapped_model_key));
    *wrap_status = locked_initial_key_wrap(test_package_model_key,
        &test_package_wrapped_model_key);
    crypto_zero(test_package_model_key, sizeof(test_package_model_key));
    *raw_zeroized = buffer_is_zero(test_package_model_key,
        sizeof(test_package_model_key));
    if ((*wrap_status != FSP_SUCCESS) || !*raw_zeroized) {
        crypto_zero(&test_package_wrapped_model_key,
            sizeof(test_package_wrapped_model_key));
        return 0;
    }

    *stage = "model-import";
    status = import_wrapped_key(&test_package_wrapped_model_key, model_handle);
    *last_status = status;
    crypto_zero(&test_package_wrapped_model_key, sizeof(test_package_wrapped_model_key));
    return status == PSA_SUCCESS;
}

static int test_package_decrypt_chunk(psa_key_handle_t model_handle,
    uint32_t chunk_index, size_t plain_bytes, size_t payload_offset,
    psa_status_t *last_status, FRESULT *fs_result, const char **stage)
{
    uint8_t nonce[12] CRYPTO_ALIGN;
    size_t aad_bytes = test_package_build_chunk_aad(chunk_index,
        (uint32_t)plain_bytes);
    size_t output_bytes = 0U;
    size_t output_capacity = decrypt_capacity(plain_bytes);
    psa_status_t status;
    int valid;

    test_package_build_chunk_nonce(nonce, chunk_index);
    *stage = "chunk-read";
    if (!test_package_read_exact(ciphertext,
        (UINT)(plain_bytes + CRYPTO_TAG_BYTES), fs_result)) {
        return 0;
    }
    *stage = "chunk-auth";
    memset(recovered, 0xa5, output_capacity);
    status = locked_aead_decrypt(model_handle, PSA_ALG_GCM,
        nonce, sizeof(nonce), test_package_aad, aad_bytes,
        ciphertext, plain_bytes + CRYPTO_TAG_BYTES,
        recovered, output_capacity, &output_bytes);
    ++crypto_diag.decrypt_count;
    crypto_diag.last_psa_status = status;
    *last_status = status;
    valid = (status == PSA_SUCCESS) && (output_bytes == plain_bytes) &&
        pattern_matches_at(recovered, plain_bytes, payload_offset);
    crypto_zero(recovered, output_capacity);
    return valid && buffer_is_zero(recovered, output_capacity);
}

static int test_package_negative_loaded(psa_key_handle_t handle,
    const uint8_t nonce[12], size_t aad_bytes, size_t plain_bytes,
    size_t damage_index, psa_status_t *last_status,
    int *output_zeroized)
{
    size_t combined_bytes = plain_bytes + CRYPTO_TAG_BYTES;
    size_t output_capacity = decrypt_capacity(plain_bytes);
    size_t output_bytes = 0U;
    psa_status_t status;
    int rejected;

    ciphertext[damage_index] ^= 1U;
    memset(recovered, 0xa5, output_capacity);
    status = locked_aead_decrypt(handle, PSA_ALG_GCM,
        nonce, 12U, test_package_aad, aad_bytes, ciphertext, combined_bytes,
        recovered, output_capacity, &output_bytes);
    ++crypto_diag.decrypt_count;
    crypto_diag.last_psa_status = status;
    *last_status = status;
    *output_zeroized = buffer_is_zero(recovered, output_capacity);
    rejected = authentication_rejected(status) && (output_bytes == 0U);
    if (authentication_rejected(status)) {
        ++crypto_diag.auth_fail_count;
    }
    crypto_zero(recovered, output_capacity);
    return rejected && *output_zeroized &&
        buffer_is_zero(recovered, output_capacity);
}

static void test_package_clear_scratch(void)
{
    crypto_zero(test_package_model_key, sizeof(test_package_model_key));
    crypto_zero(&test_package_wrapped_model_key, sizeof(test_package_wrapped_model_key));
    crypto_zero(test_package_aad, sizeof(test_package_aad));
    crypto_zero(test_package_manifest, sizeof(test_package_manifest));
    crypto_zero(ciphertext, sizeof(ciphertext));
    crypto_zero(recovered, sizeof(recovered));
}

static int run_package_integration_suite(void)
{
    mtfs_ra8p1_key_metadata_t metadata = {0U};
    mtfs_ra8p1_key_store_diagnostics_t store_diag;
    psa_key_handle_t fleet_handle = 0U;
    psa_key_handle_t model_handle = 0U;
    psa_status_t status = PSA_SUCCESS;
    fsp_err_t wrap_status = FSP_SUCCESS;
    FRESULT fs_result = FR_OK;
    const char *stage = "open";
    int raw_zeroized = 0;
    int failed = 0;

    tm_printf((UB *)"[crypto-package-test] source=%s fleet-specific package bytes=%u chunks=%u\n",
        (UB *)TEST_PACKAGE_PATH, (UW)TEST_PACKAGE_BYTES,
        (UW)TEST_PACKAGE_CHUNK_COUNT);
    if (!test_package_open(&stage, &fs_result)) {
        tm_printf((UB *)"[crypto-package-test] FAIL stage=%s fatfs=%d\n",
            (UB *)stage, (INT)fs_result);
        return 1;
    }
    if (!prepare_key(&fleet_handle, &metadata, &store_diag)) {
        test_package_close();
        test_package_clear_scratch();
        return 1;
    }
    if (metadata.key_id != read_le32(&test_package_manifest[32])) {
        uint32_t package_key_id = read_le32(&test_package_manifest[32]);

        destroy_key(&fleet_handle);
        test_package_close();
        test_package_clear_scratch();
        tm_printf((UB *)"[crypto-package-test] FAIL stage=key-id package=%u ospi=%u\n",
            (UW)package_key_id, (UW)metadata.key_id);
        return 1;
    }
    tm_printf((UB *)"[crypto-package-test] selector package-key=%u/%u ospi-key=%u/%u version-policy=Phase-4.2\n",
        (UW)read_le32(&test_package_manifest[32]),
        (UW)read_le32(&test_package_manifest[36]),
        (UW)metadata.key_id, (UW)metadata.key_version);
    if (!test_package_open_model_key(fleet_handle, &model_handle, &status,
        &wrap_status, &fs_result, &stage, &raw_zeroized)) {
        destroy_key(&fleet_handle);
        test_package_close();
        test_package_clear_scratch();
        if ((strcmp(stage, "envelope-auth") == 0) &&
            authentication_rejected(status)) {
            tm_printf((UB *)"[crypto-package-test] FAIL stage=fleet-key-mismatch psa=%d; regenerate MTFSTEST.MTF with the provisioned fleet.key\n",
                (INT)status);
        } else {
            tm_printf((UB *)"[crypto-package-test] FAIL stage=%s fatfs=%d psa=%d fsp=%d raw-k-model-zeroize=%s\n",
                (UB *)stage, (INT)fs_result, (INT)status,
                (INT)wrap_status,
                raw_zeroized ? (UB *)"PASS" : (UB *)"FAIL");
        }
        return 1;
    }
    destroy_key(&fleet_handle);
    tm_printf((UB *)"[crypto-package-test] envelope PASS model-wrap=PASS raw-k-model-zeroize=%s\n",
        raw_zeroized ? (UB *)"PASS" : (UB *)"FAIL");

    stage = "chunk-0";
    if (!test_package_decrypt_chunk(model_handle, 0U, TEST_PACKAGE_CHUNK0_PLAIN_BYTES,
        0U, &status, &fs_result, &stage)) {
        failed = 1;
    } else {
        tm_printf((UB *)"[crypto-package-test] payload chunk=0 bytes=%u PASS\n",
            (UW)TEST_PACKAGE_CHUNK0_PLAIN_BYTES);
    }
    stage = "chunk-1";
    if (!failed && !test_package_decrypt_chunk(model_handle, 1U,
        TEST_PACKAGE_CHUNK1_PLAIN_BYTES, TEST_PACKAGE_CHUNK0_PLAIN_BYTES,
        &status, &fs_result, &stage)) {
        failed = 1;
    } else if (!failed) {
        tm_printf((UB *)"[crypto-package-test] payload chunk=1 bytes=%u PASS\n",
            (UW)TEST_PACKAGE_CHUNK1_PLAIN_BYTES);
    }
    destroy_key(&model_handle);
    test_package_close();
    test_package_clear_scratch();
    if (failed) {
        tm_printf((UB *)"[crypto-package-test] fleet envelope/wrap/payload FAIL stage=%s fatfs=%d psa=%d fsp=%d\n",
            (UB *)stage, (INT)fs_result, (INT)status,
            (INT)wrap_status);
        return 1;
    }
    tm_printf((UB *)"[crypto-package-test] fleet envelope/wrap/payload PASS scratch-zeroize=PASS key-id=%u ospi-key-version=%u\n",
        (UW)metadata.key_id, (UW)metadata.key_version);
    return 0;
}

static int run_package_negative_suite(void)
{
    mtfs_ra8p1_key_metadata_t metadata = {0U};
    mtfs_ra8p1_key_store_diagnostics_t store_diag;
    psa_key_handle_t fleet_handle = 0U;
    psa_key_handle_t model_handle = 0U;
    psa_status_t status = PSA_SUCCESS;
    psa_status_t envelope_status = PSA_SUCCESS;
    psa_status_t cipher_status = PSA_SUCCESS;
    psa_status_t tag_status = PSA_SUCCESS;
    fsp_err_t wrap_status = FSP_SUCCESS;
    FRESULT fs_result = FR_OK;
    const char *stage = "open";
    uint8_t nonce[12] CRYPTO_ALIGN;
    size_t aad_bytes;
    int raw_zeroized = 0;
    int envelope_zeroized = 0;
    int cipher_zeroized = 0;
    int tag_zeroized = 0;
    int envelope_rejected = 0;
    int cipher_rejected = 0;
    int tag_rejected = 0;
    int baseline_ok = 0;

    tm_printf((UB *)"[crypto-negative] source=%s mutation=in-RAM SD-unchanged\n",
        (UB *)TEST_PACKAGE_PATH);
    if (!test_package_open(&stage, &fs_result) ||
        !prepare_key(&fleet_handle, &metadata, &store_diag)) {
        test_package_close();
        test_package_clear_scratch();
        tm_printf((UB *)"[crypto-negative] FAIL stage=%s fatfs=%d\n",
            (UB *)stage, (INT)fs_result);
        return 1;
    }

    if (test_package_seek(TEST_PACKAGE_MANIFEST_BYTES, &stage, &fs_result) &&
        test_package_open_model_key(fleet_handle, &model_handle, &status,
            &wrap_status, &fs_result, &stage, &raw_zeroized)) {
        aad_bytes = test_package_build_envelope_aad();
        memcpy(nonce, &test_package_manifest[TEST_PACKAGE_KEY_NONCE_OFFSET], sizeof(nonce));
        if (test_package_seek(TEST_PACKAGE_MANIFEST_BYTES, &stage, &fs_result) &&
            test_package_read_exact(ciphertext, TEST_PACKAGE_ENVELOPE_BYTES, &fs_result)) {
            envelope_rejected = test_package_negative_loaded(fleet_handle, nonce,
                aad_bytes, TEST_PACKAGE_ENVELOPE_PLAIN_BYTES,
                TEST_PACKAGE_ENVELOPE_BYTES - 1U, &envelope_status,
                &envelope_zeroized);
        }
        destroy_key(&fleet_handle);
        if (test_package_seek(TEST_PACKAGE_MANIFEST_BYTES + TEST_PACKAGE_ENVELOPE_BYTES,
                &stage, &fs_result) &&
            test_package_decrypt_chunk(model_handle, 0U,
                TEST_PACKAGE_CHUNK0_PLAIN_BYTES, 0U, &status, &fs_result,
                &stage) &&
            test_package_seek(TEST_PACKAGE_MANIFEST_BYTES + TEST_PACKAGE_ENVELOPE_BYTES,
                &stage, &fs_result) &&
            test_package_read_exact(ciphertext,
                TEST_PACKAGE_CHUNK0_PLAIN_BYTES + CRYPTO_TAG_BYTES,
                &fs_result)) {
            aad_bytes = test_package_build_chunk_aad(0U,
                TEST_PACKAGE_CHUNK0_PLAIN_BYTES);
            test_package_build_chunk_nonce(nonce, 0U);
            cipher_rejected = test_package_negative_loaded(model_handle, nonce,
                aad_bytes, TEST_PACKAGE_CHUNK0_PLAIN_BYTES, 0U,
                &cipher_status, &cipher_zeroized);
        }
        if (test_package_seek(TEST_PACKAGE_MANIFEST_BYTES + TEST_PACKAGE_ENVELOPE_BYTES +
                TEST_PACKAGE_CHUNK0_PLAIN_BYTES + CRYPTO_TAG_BYTES,
                &stage, &fs_result) &&
            test_package_decrypt_chunk(model_handle, 1U,
                TEST_PACKAGE_CHUNK1_PLAIN_BYTES, TEST_PACKAGE_CHUNK0_PLAIN_BYTES,
                &status, &fs_result, &stage) &&
            test_package_seek(TEST_PACKAGE_MANIFEST_BYTES + TEST_PACKAGE_ENVELOPE_BYTES +
                TEST_PACKAGE_CHUNK0_PLAIN_BYTES + CRYPTO_TAG_BYTES,
                &stage, &fs_result) &&
            test_package_read_exact(ciphertext,
                TEST_PACKAGE_CHUNK1_PLAIN_BYTES + CRYPTO_TAG_BYTES,
                &fs_result)) {
            aad_bytes = test_package_build_chunk_aad(1U,
                TEST_PACKAGE_CHUNK1_PLAIN_BYTES);
            test_package_build_chunk_nonce(nonce, 1U);
            tag_rejected = test_package_negative_loaded(model_handle, nonce,
                aad_bytes, TEST_PACKAGE_CHUNK1_PLAIN_BYTES,
                TEST_PACKAGE_CHUNK1_PLAIN_BYTES + CRYPTO_TAG_BYTES - 1U,
                &tag_status, &tag_zeroized);
        }
        baseline_ok = envelope_rejected && cipher_rejected &&
            tag_rejected;
    }
    destroy_key(&fleet_handle);
    destroy_key(&model_handle);
    test_package_close();
    test_package_clear_scratch();

    tm_printf((UB *)"[crypto-negative] package envelope-tag-tamper=%s chunk-ciphertext-tamper=%s chunk-tag-tamper=%s output-zeroize=%s overall=%s status=%d/%d/%d\n",
        envelope_rejected ? (UB *)"PASS" : (UB *)"FAIL",
        cipher_rejected ? (UB *)"PASS" : (UB *)"FAIL",
        tag_rejected ? (UB *)"PASS" : (UB *)"FAIL",
        (envelope_zeroized && cipher_zeroized && tag_zeroized &&
            raw_zeroized) ? (UB *)"PASS" : (UB *)"FAIL",
        (baseline_ok &&
            envelope_zeroized && cipher_zeroized && tag_zeroized &&
            raw_zeroized) ? (UB *)"PASS" : (UB *)"FAIL",
        (INT)envelope_status, (INT)cipher_status, (INT)tag_status);
    return (baseline_ok &&
        envelope_zeroized && cipher_zeroized && tag_zeroized &&
        raw_zeroized) ? 0 : 1;
}

static void print_info(void)
{
    rsip_aes_wrapped_key_t wrapped_key;
    mtfs_ra8p1_key_metadata_t metadata;
    mtfs_ra8p1_key_store_diagnostics_t store_diag;
    mtfs_ra8p1_key_store_status_t store_status;

    store_status = load_key(&wrapped_key, &metadata, &store_diag);
    tm_printf((UB *)"[crypto] mode=RSIP-E50D Compatibility FSP=6.5.0 AES-256-GCM=enabled\n");
    tm_printf((UB *)"[crypto] psa-buffers=exclusive flat-build-only bsp-heap-bytes=12288\n");
    tm_printf((UB *)"[crypto] wrapped-key=HUK-bound blob-bytes=%u store=onboard-OSPI status=%s valid-slots=%u\n",
        (UW)MTFS_RA8P1_AES256_WRAPPED_BYTES,
        (UB *)mtfs_ra8p1_key_store_status_string(store_status),
        (UW)store_diag.valid_slots);
    if (store_status == MTFS_RA8P1_KEY_STORE_OK) {
        tm_printf((UB *)"[crypto] key-id=%u key-version=%u generation=%u slot=0x%08x\n",
            (UW)metadata.key_id, (UW)metadata.key_version,
            (UW)metadata.generation, (UW)metadata.slot_offset);
    } else {
        tm_printf((UB *)"[crypto] provisioning=BLOCKED; run dedicated key-provision app\n");
    }
    tm_printf((UB *)"[crypto] init=%u import=%u destroy=%u encrypt=%u decrypt=%u auth-fail=%u zeroize=%u last-psa=%d last-fsp=%d\n",
        (UW)crypto_diag.initialize_count, (UW)crypto_diag.import_count,
        (UW)crypto_diag.destroy_count, (UW)crypto_diag.encrypt_count,
        (UW)crypto_diag.decrypt_count, (UW)crypto_diag.auth_fail_count,
        (UW)crypto_diag.zeroize_count, (INT)crypto_diag.last_psa_status,
        (INT)store_diag.last_fsp_error);
    mtfs_ra8p1_ospi_key_store_zero(&wrapped_key, sizeof(wrapped_key));
}
#endif

void mtfs_ra8p1_crypto_spike_banner(void)
{
#if MTFS_RA8P1_CRYPTO_SPIKE_ENABLE
    tm_printf((UB *)"[crypto] Compatibility Mode; HUK-wrapped fleet key source=onboard OSPI\n");
#endif
}

int mtfs_ra8p1_crypto_spike_command(const char *line)
{
#if MTFS_RA8P1_CRYPTO_SPIKE_ENABLE
    if (strcmp(line, "crypto-info") == 0) {
        print_info();
        return 1;
    }
    if (strcmp(line, "crypto-consistency") == 0) {
        (void)run_positive_suite();
        return 1;
    }
    if (strcmp(line, "crypto-negative") == 0) {
        (void)run_package_negative_suite();
        return 1;
    }
    if (strcmp(line, "crypto-package-test") == 0) {
        (void)run_package_integration_suite();
        return 1;
    }
#else
    (void)line;
#endif
    return 0;
}
