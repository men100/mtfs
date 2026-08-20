/* RSIP Compatibility Mode provisioned-key validation commands. */

#include "mtfs_ra8p1_crypto_spike.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tm/tmonitor.h>

#if MTFS_RA8P1_CRYPTO_SPIKE_ENABLE
#include "hal_data.h"
#include "mbedtls/platform.h"
#include "psa/crypto.h"
#include "mtfs_ra8p1_ospi_key_store.h"

#if !defined(MBEDTLS_PSA_ASSUME_EXCLUSIVE_BUFFERS)
#error "The RA8P1 flat-build crypto tests require exclusive PSA buffers"
#endif

#define CRYPTO_ALIGN        __attribute__((aligned(16)))
#define CRYPTO_MAX_BYTES    (64U * 1024U)
#define CRYPTO_TAG_BYTES    (16U)

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
static uint8_t ciphertext[CRYPTO_MAX_BYTES + CRYPTO_TAG_BYTES] CRYPTO_ALIGN;
/*
 * FSP 6.5.0's SCE GCM final primitive writes one complete final block at
 * floor(payload_bytes / 16) * 16, including when the payload is block-aligned.
 */
static uint8_t recovered[CRYPTO_MAX_BYTES + CRYPTO_TAG_BYTES] CRYPTO_ALIGN;
static mbedtls_platform_context platform_context;
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

static psa_status_t crypto_initialize(void)
{
    psa_status_t status;

    if (crypto_ready != 0U) {
        return PSA_SUCCESS;
    }
    if (mbedtls_platform_setup(&platform_context) != 0) {
        crypto_diag.last_psa_status = PSA_ERROR_HARDWARE_FAILURE;
        return PSA_ERROR_HARDWARE_FAILURE;
    }
    status = psa_crypto_init();
    crypto_diag.last_psa_status = status;
    if (status != PSA_SUCCESS) {
        mbedtls_platform_teardown(&platform_context);
        return status;
    }
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
    status = psa_import_key(&attributes,
        (const uint8_t *)wrapped_key->value,
        MTFS_RA8P1_AES256_WRAPPED_BYTES, handle);
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
        crypto_diag.last_psa_status = psa_destroy_key(*handle);
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
        status = psa_aead_encrypt(handle, PSA_ALG_GCM,
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
        status = psa_aead_decrypt(handle, PSA_ALG_GCM,
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
        status = psa_aead_encrypt(handle, PSA_ALG_GCM,
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
            status = psa_aead_decrypt(handle, PSA_ALG_GCM,
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

static int decrypt_rejects(psa_key_handle_t handle,
    const uint8_t nonce[12], const uint8_t *aad, size_t aad_bytes,
    size_t cipher_bytes, psa_status_t *rejection_status,
    int *output_zeroized)
{
    psa_status_t status;
    size_t output_bytes = 0U;
    size_t payload_bytes = cipher_bytes - CRYPTO_TAG_BYTES;
    size_t recovered_capacity = decrypt_capacity(payload_bytes);

    memset(recovered, 0xa5, recovered_capacity);
    status = psa_aead_decrypt(handle, PSA_ALG_GCM,
        nonce, 12U, aad, aad_bytes, ciphertext, cipher_bytes,
        recovered, recovered_capacity, &output_bytes);
    ++crypto_diag.decrypt_count;
    crypto_diag.last_psa_status = status;
    *rejection_status = status;
    *output_zeroized = buffer_is_zero(recovered, recovered_capacity);
    if (authentication_rejected(status)) {
        ++crypto_diag.auth_fail_count;
    }
    crypto_zero(recovered, recovered_capacity);
    return authentication_rejected(status) && (output_bytes == 0U) &&
        *output_zeroized;
}

static int run_negative_case(int damage_tag,
    const uint8_t nonce[12], const uint8_t *aad, size_t aad_bytes,
    mtfs_ra8p1_key_metadata_t *metadata, psa_status_t *rejection_status,
    int *output_zeroized)
{
    mtfs_ra8p1_key_store_diagnostics_t store_diag;
    psa_key_handle_t handle = 0U;
    psa_status_t status;
    size_t cipher_bytes = 0U;
    size_t plain_bytes = 0U;
    size_t recovered_capacity = decrypt_capacity(37U);
    int rejected = 0;

    if (!prepare_key(&handle, metadata, &store_diag)) {
        *rejection_status = crypto_diag.last_psa_status;
        return 0;
    }
    fill_pattern(plaintext, 37U);
    status = psa_aead_encrypt(handle, PSA_ALG_GCM,
        nonce, 12U, aad, aad_bytes, plaintext, 37U,
        ciphertext, 37U + CRYPTO_TAG_BYTES, &cipher_bytes);
    ++crypto_diag.encrypt_count;
    crypto_diag.last_psa_status = status;
    if (status == PSA_SUCCESS) {
        memset(recovered, 0xa5, recovered_capacity);
        status = psa_aead_decrypt(handle, PSA_ALG_GCM,
            nonce, 12U, aad, aad_bytes, ciphertext, cipher_bytes,
            recovered, recovered_capacity, &plain_bytes);
        ++crypto_diag.decrypt_count;
        crypto_diag.last_psa_status = status;
    }
    if ((status == PSA_SUCCESS) && (plain_bytes == 37U) &&
        pattern_matches(recovered, 37U)) {
        size_t damage_offset = damage_tag ? cipher_bytes - 1U : 0U;

        ciphertext[damage_offset] ^= 1U;
        rejected = decrypt_rejects(handle, nonce, aad, aad_bytes,
            cipher_bytes, rejection_status, output_zeroized);
        ciphertext[damage_offset] ^= 1U;
    } else {
        *rejection_status = status;
        tm_printf((UB *)"[crypto] negative baseline FAIL mutation=%s psa=%d plain-bytes=%u\n",
            damage_tag ? (UB *)"tag" : (UB *)"ciphertext",
            (INT)status, (UW)plain_bytes);
    }
    destroy_key(&handle);
    return rejected;
}

static int run_negative_suite(void)
{
    static const uint8_t aad[] = {
        'm','i','c','r','o','T','-','F','S',' ','n','e','g'
    };
    static const uint8_t nonce[12] CRYPTO_ALIGN = {
        0x4dU, 0x54U, 0x46U, 0x53U, 0x2dU, 0x4eU,
        0x45U, 0x47U, 0x2dU, 0x30U, 0x30U, 0x31U
    };
    mtfs_ra8p1_key_metadata_t metadata = {0U};
    psa_status_t tag_status = PSA_ERROR_GENERIC_ERROR;
    psa_status_t cipher_status = PSA_ERROR_GENERIC_ERROR;
    int tag_rejected = 0;
    int cipher_rejected = 0;
    int tag_zeroized = 0;
    int cipher_zeroized = 0;

    tag_rejected = run_negative_case(1, nonce, aad, sizeof(aad),
        &metadata, &tag_status, &tag_zeroized);
    cipher_rejected = run_negative_case(0, nonce, aad, sizeof(aad),
        &metadata, &cipher_status, &cipher_zeroized);
    crypto_zero(plaintext, sizeof(plaintext));
    crypto_zero(ciphertext, sizeof(ciphertext));
    crypto_zero(recovered, sizeof(recovered));
    tm_printf((UB *)"[crypto] provisioned-key GCM negative tag=%s ciphertext=%s output-zeroize=%s %s key-version=%u status=%d/%d\n",
        tag_rejected ? (UB *)"REJECT" : (UB *)"FAIL",
        cipher_rejected ? (UB *)"REJECT" : (UB *)"FAIL",
        (tag_zeroized && cipher_zeroized) ? (UB *)"PASS" : (UB *)"FAIL",
        (tag_rejected && cipher_rejected) ? (UB *)"PASS" : (UB *)"FAIL",
        (UW)metadata.key_version, (INT)tag_status, (INT)cipher_status);
    return (tag_rejected && cipher_rejected) ? 0 : 1;
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
        (void)run_negative_suite();
        return 1;
    }
#else
    (void)line;
#endif
    return 0;
}
