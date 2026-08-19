/*
 * TEST ONLY - NOT FOR PRODUCTION.
 *
 * This target-local hardware spike deliberately contains no fleet test key.
 * FSP 6.5.0 does not expose a supported plaintext-key import path from the
 * RSIP-E50D Protected Mode stack; see docs/security/ek-ra8p1-rsip-e50d-spike.md.
 */

#include "mtfs_ra8p1_crypto_spike.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tm/tmonitor.h>

#if MTFS_RA8P1_CRYPTO_SPIKE_ENABLE
#include "hal_data.h"

#define MTFS_CRYPTO_ALIGN __attribute__((aligned(16)))
#define MTFS_CRYPTO_SCRATCH_BYTES (64U * 1024U)
#define MTFS_CRYPTO_TAG_BYTES     (16U)
#define MTFS_CRYPTO_KEY_BYTES     (32U)
#define MTFS_CRYPTO_WRAPPED_BYTES (52U)
#define MTFS_CRYPTO_IO_SLICE_BYTES (4096U)

typedef struct mtfs_crypto_spike_diagnostics {
    uint32_t open_count;
    uint32_t close_count;
    uint32_t key_generate_success;
    uint32_t gcm_start_count;
    uint32_t gcm_update_count;
    uint32_t gcm_verify_count;
    uint32_t authentication_failure_count;
    uint32_t abort_count;
    uint32_t zeroize_count;
    uint32_t stale_handle_rejection_count;
    uint32_t nvm_read_count;
    uint32_t nvm_write_count;
    uint32_t nvm_erase_count;
    uint32_t generation;
    uint32_t last_fsp_error;
} mtfs_crypto_spike_diagnostics_t;

static uint8_t crypto_scratch[MTFS_CRYPTO_SCRATCH_BYTES] MTFS_CRYPTO_ALIGN;
static uint8_t crypto_ciphertext[MTFS_CRYPTO_SCRATCH_BYTES] MTFS_CRYPTO_ALIGN;
static uint8_t crypto_destination[MTFS_CRYPTO_SCRATCH_BYTES + 16U] MTFS_CRYPTO_ALIGN;
static uint8_t wrapped_key_value[MTFS_CRYPTO_WRAPPED_BYTES] MTFS_CRYPTO_ALIGN;
static mtfs_crypto_spike_diagnostics_t crypto_diag;
static int crypto_is_open;

static void crypto_zeroize(void *buffer, size_t length)
{
    volatile uint8_t *p = (volatile uint8_t *)buffer;
    while (length != 0U) {
        *p++ = 0U;
        --length;
    }
    __asm volatile ("" : : "r" (buffer) : "memory");
    ++crypto_diag.zeroize_count;
}

static fsp_err_t crypto_open(void)
{
    fsp_err_t error;
    if (crypto_is_open) {
        return FSP_SUCCESS;
    }
    error = R_RSIP_Open(g_rsip.p_ctrl, g_rsip.p_cfg);
    crypto_diag.last_fsp_error = (uint32_t)error;
    if (error == FSP_SUCCESS) {
        crypto_is_open = 1;
        ++crypto_diag.open_count;
        ++crypto_diag.generation;
    }
    return error;
}

static void crypto_close(void)
{
    if (crypto_is_open) {
        fsp_err_t error = R_RSIP_Close(g_rsip.p_ctrl);
        crypto_diag.last_fsp_error = (uint32_t)error;
        if (error == FSP_SUCCESS) {
            crypto_is_open = 0;
            ++crypto_diag.close_count;
        }
    }
}

static fsp_err_t crypto_abort_and_reopen(void)
{
    ++crypto_diag.abort_count;
    crypto_close();
    return crypto_open();
}

static fsp_err_t crypto_encrypt(const rsip_wrapped_key_t *key,
    const uint8_t *nonce, const uint8_t *aad, uint32_t aad_length,
    const uint8_t *plaintext, uint32_t plaintext_length,
    uint8_t *ciphertext, uint8_t *tag)
{
    uint32_t input_offset = 0U;
    uint32_t output_offset = 0U;
    uint32_t finish_length = 0U;
    fsp_err_t error = R_RSIP_AES_AEAD_Init(g_rsip.p_ctrl,
        RSIP_AES_AEAD_MODE_GCM_ENC, key, nonce, 12U);
    ++crypto_diag.gcm_start_count;
    /* FSP 6.5.0 LengthsSet is CCM-only; GCM fixes the tag at 16 bytes. */
    if ((error == FSP_SUCCESS) && (aad_length != 0U)) {
        uint32_t first_aad = aad_length > 8U ? 8U : aad_length;
        error = R_RSIP_AES_AEAD_AADUpdate(g_rsip.p_ctrl, aad, first_aad);
        if ((error == FSP_SUCCESS) && (first_aad != aad_length)) {
            error = R_RSIP_AES_AEAD_AADUpdate(g_rsip.p_ctrl,
                aad + first_aad, aad_length - first_aad);
        }
    }
    while ((error == FSP_SUCCESS) && (input_offset < plaintext_length)) {
        uint32_t slice = plaintext_length - input_offset;
        uint32_t update_length = 0U;
        if (slice > MTFS_CRYPTO_IO_SLICE_BYTES) {
            slice = MTFS_CRYPTO_IO_SLICE_BYTES;
        }
        error = R_RSIP_AES_AEAD_Update(g_rsip.p_ctrl,
            plaintext + input_offset, slice, ciphertext + output_offset,
            &update_length);
        ++crypto_diag.gcm_update_count;
        input_offset += slice;
        output_offset += update_length;
    }
    if (error == FSP_SUCCESS) {
        error = R_RSIP_AES_AEAD_Finish(g_rsip.p_ctrl,
            ciphertext + output_offset, &finish_length, tag);
    }
    if ((error == FSP_SUCCESS) &&
        ((output_offset + finish_length) != plaintext_length)) {
        error = FSP_ERR_CRYPTO_UNKNOWN;
    }
    crypto_diag.last_fsp_error = (uint32_t)error;
    return error;
}

static fsp_err_t crypto_decrypt_private(const rsip_wrapped_key_t *key,
    const uint8_t *nonce, const uint8_t *aad, uint32_t aad_length,
    const uint8_t *ciphertext, uint32_t ciphertext_length,
    const uint8_t *tag, uint8_t *destination, uint32_t destination_length)
{
    uint32_t input_offset = 0U;
    uint32_t output_offset = 0U;
    uint32_t finish_length = 0U;
    fsp_err_t error;

    if ((ciphertext_length > MTFS_CRYPTO_SCRATCH_BYTES) ||
        (destination_length < ciphertext_length)) {
        return FSP_ERR_INVALID_SIZE;
    }
    crypto_zeroize(crypto_scratch, MTFS_CRYPTO_SCRATCH_BYTES);
    error = R_RSIP_AES_AEAD_Init(g_rsip.p_ctrl,
        RSIP_AES_AEAD_MODE_GCM_DEC, key, nonce, 12U);
    ++crypto_diag.gcm_start_count;
    /* FSP 6.5.0 LengthsSet is CCM-only; GCM fixes the tag at 16 bytes. */
    if ((error == FSP_SUCCESS) && (aad_length != 0U)) {
        uint32_t first_aad = aad_length > 8U ? 8U : aad_length;
        error = R_RSIP_AES_AEAD_AADUpdate(g_rsip.p_ctrl, aad, first_aad);
        if ((error == FSP_SUCCESS) && (first_aad != aad_length)) {
            error = R_RSIP_AES_AEAD_AADUpdate(g_rsip.p_ctrl,
                aad + first_aad, aad_length - first_aad);
        }
    }
    while ((error == FSP_SUCCESS) && (input_offset < ciphertext_length)) {
        uint32_t slice = ciphertext_length - input_offset;
        uint32_t update_length = 0U;
        if (slice > MTFS_CRYPTO_IO_SLICE_BYTES) {
            slice = MTFS_CRYPTO_IO_SLICE_BYTES;
        }
        error = R_RSIP_AES_AEAD_Update(g_rsip.p_ctrl,
            ciphertext + input_offset, slice, crypto_scratch + output_offset,
            &update_length);
        ++crypto_diag.gcm_update_count;
        input_offset += slice;
        output_offset += update_length;
    }
    if (error == FSP_SUCCESS) {
        error = R_RSIP_AES_AEAD_Verify(g_rsip.p_ctrl,
            crypto_scratch + output_offset, &finish_length, tag,
            MTFS_CRYPTO_TAG_BYTES);
        ++crypto_diag.gcm_verify_count;
    }
    if (error == FSP_SUCCESS) {
        if ((output_offset + finish_length) == ciphertext_length) {
            memcpy(destination, crypto_scratch, ciphertext_length);
        } else {
            error = FSP_ERR_CRYPTO_UNKNOWN;
        }
    } else {
        if (error == FSP_ERR_CRYPTO_RSIP_AUTHENTICATION) {
            ++crypto_diag.authentication_failure_count;
        }
        crypto_zeroize(destination, ciphertext_length);
    }
    crypto_zeroize(crypto_scratch, MTFS_CRYPTO_SCRATCH_BYTES);
    crypto_diag.last_fsp_error = (uint32_t)error;
    return error;
}

static void crypto_fill_pattern(uint8_t *buffer, uint32_t length)
{
    for (uint32_t i = 0U; i < length; ++i) {
        buffer[i] = (uint8_t)((i * 7U + 3U) & 0xffU);
    }
}

static int crypto_pattern_matches(const uint8_t *buffer, uint32_t length)
{
    for (uint32_t i = 0U; i < length; ++i) {
        if (buffer[i] != (uint8_t)((i * 7U + 3U) & 0xffU)) {
            return 0;
        }
    }
    return 1;
}

static int crypto_run_protected_roundtrip(void)
{
    static const uint8_t nonce_prefix[11] MTFS_CRYPTO_ALIGN = {
        0x10U, 0x22U, 0x34U, 0x46U, 0x58U, 0x6aU,
        0x7cU, 0x8eU, 0x90U, 0xa2U, 0xb4U
    };
    static const uint8_t aad[19] MTFS_CRYPTO_ALIGN = {
        'm','i','c','r','o','T','-','F','S',' ','R','S','I','P',' ','s','p','i','k'
    };
    static const uint32_t lengths[] = {
        0U, 37U, 4U * 1024U, 16U * 1024U, 64U * 1024U
    };
    uint8_t nonce[12] MTFS_CRYPTO_ALIGN;
    uint8_t tag[MTFS_CRYPTO_TAG_BYTES] MTFS_CRYPTO_ALIGN = {0U};
    uint8_t bad_tag[MTFS_CRYPTO_TAG_BYTES] MTFS_CRYPTO_ALIGN = {0U};
    rsip_wrapped_key_t key = { RSIP_KEY_TYPE_AES_256, wrapped_key_value };
    fsp_err_t error;
    int failed = 0;

    memcpy(nonce, nonce_prefix, sizeof(nonce_prefix));
    error = crypto_open();
    if (error == FSP_SUCCESS) {
        error = R_RSIP_KeyGenerate(g_rsip.p_ctrl, &key);
        crypto_diag.last_fsp_error = (uint32_t)error;
        if (error == FSP_SUCCESS) {
            ++crypto_diag.key_generate_success;
        }
    }
    for (size_t case_index = 0U;
         (error == FSP_SUCCESS) && (case_index < (sizeof(lengths) / sizeof(lengths[0])));
         ++case_index) {
        uint32_t length = lengths[case_index];
        nonce[11] = (uint8_t)(0x40U + case_index);
        crypto_fill_pattern(crypto_destination, length);
        error = crypto_encrypt(&key, nonce, aad, sizeof(aad),
            crypto_destination, length, crypto_ciphertext, tag);
        memset(crypto_destination, 0xa5, length + 1U);
        if (error == FSP_SUCCESS) {
            error = crypto_decrypt_private(&key, nonce, aad, sizeof(aad),
                crypto_ciphertext, length, tag, crypto_destination,
                sizeof(crypto_destination));
        }
        if ((error != FSP_SUCCESS) ||
            !crypto_pattern_matches(crypto_destination, length) ||
            (crypto_destination[length] != 0xa5U)) {
            failed = 1;
        }
    }

    nonce[11] = 0xe0U;
    crypto_fill_pattern(crypto_destination, 37U);
    error = crypto_encrypt(&key, nonce, aad, sizeof(aad), crypto_destination,
        37U, crypto_ciphertext, tag);
    memcpy(bad_tag, tag, sizeof(bad_tag));
    bad_tag[0] ^= 1U;
    memset(crypto_destination, 0xa5, 38U);
    if (error == FSP_SUCCESS) {
        error = crypto_decrypt_private(&key, nonce, aad, sizeof(aad),
        crypto_ciphertext, 37U, bad_tag, crypto_destination,
        sizeof(crypto_destination));
    }
    if (error != FSP_ERR_CRYPTO_RSIP_AUTHENTICATION) {
        failed = 1;
    }
    for (size_t i = 0U; i < 37U; ++i) {
        if (crypto_destination[i] != 0U) {
            failed = 1;
        }
    }
    if (crypto_destination[37U] != 0xa5U) {
        failed = 1;
    }

    if (crypto_abort_and_reopen() != FSP_SUCCESS) {
        failed = 1;
    } else {
        memset(crypto_destination, 0xa5, 38U);
        error = crypto_decrypt_private(&key, nonce, aad, sizeof(aad),
            crypto_ciphertext, 37U, tag, crypto_destination,
            sizeof(crypto_destination));
        if ((error != FSP_SUCCESS) ||
            !crypto_pattern_matches(crypto_destination, 37U)) {
            failed = 1;
        }
    }

    crypto_close();
    crypto_zeroize(wrapped_key_value, sizeof(wrapped_key_value));
    crypto_zeroize(crypto_ciphertext, sizeof(crypto_ciphertext));
    crypto_zeroize(tag, sizeof(tag));
    crypto_zeroize(bad_tag, sizeof(bad_tag));
    crypto_zeroize(crypto_destination, sizeof(crypto_destination));
    tm_printf((UB *)"[crypto] protected generated-key GCM empty/partial/4KiB/16KiB/64KiB/multishot/negative/reopen %s; fixed-key KAT BLOCKED\n",
        failed ? (UB *)"FAIL" : (UB *)"PASS");
    return failed ? 1 : 0;
}

static void crypto_print_info(void)
{
    tm_printf((UB *)"[crypto] TEST ONLY - NOT FOR PRODUCTION\n");
    tm_printf((UB *)"[crypto] mode=RSIP-E50D Protected FSP=6.5.0 AES-256-GCM=enabled flat-build\n");
    tm_printf((UB *)"[crypto] wrapped_type=AES-256 blob_bytes=%u alignment=16 scratch_bytes=%u alignment=16\n",
        (UW)MTFS_CRYPTO_WRAPPED_BYTES, (UW)MTFS_CRYPTO_SCRATCH_BYTES);
    tm_printf((UB *)"[crypto] provisioning=BLOCKED key_id=none key_version=none test_key_linked=no\n");
    tm_printf((UB *)"[crypto] test_nvm=UNAVAILABLE selected-device-data-flash-bytes=0 writes=%u erases=%u\n",
        (UW)crypto_diag.nvm_write_count, (UW)crypto_diag.nvm_erase_count);
    tm_printf((UB *)"[crypto] open=%u close=%u wrap=%u start=%u update=%u verify=%u auth_fail=%u abort=%u zeroize=%u generation=%u last_fsp=%u\n",
        (UW)crypto_diag.open_count, (UW)crypto_diag.close_count,
        (UW)crypto_diag.key_generate_success, (UW)crypto_diag.gcm_start_count,
        (UW)crypto_diag.gcm_update_count, (UW)crypto_diag.gcm_verify_count,
        (UW)crypto_diag.authentication_failure_count, (UW)crypto_diag.abort_count,
        (UW)crypto_diag.zeroize_count, (UW)crypto_diag.generation,
        (UW)crypto_diag.last_fsp_error);
}

static int crypto_is_blocked_command(const char *line)
{
    return strcmp(line, "crypto-provision-test-key") == 0 ||
        strcmp(line, "crypto-reboot-check") == 0 ||
        strcmp(line, "crypto-powercycle-check") == 0 ||
        strcmp(line, "crypto-clear-test-key") == 0 ||
        strcmp(line, "crypto-golden") == 0 ||
        strcmp(line, "crypto-bench") == 0 ||
        strcmp(line, "crypto-sd") == 0 ||
        strcmp(line, "crypto-run") == 0;
}
#endif

void mtfs_ra8p1_crypto_spike_banner(void)
{
#if MTFS_RA8P1_CRYPTO_SPIKE_ENABLE
    tm_printf((UB *)"[crypto] TEST KEY - NOT FOR PRODUCTION (no test key linked: provisioning blocked)\n");
#endif
}

int mtfs_ra8p1_crypto_spike_command(const char *line)
{
#if MTFS_RA8P1_CRYPTO_SPIKE_ENABLE
    if (strcmp(line, "crypto-info") == 0) {
        crypto_print_info();
        return 1;
    }
    if ((strcmp(line, "crypto-kat") == 0) ||
        (strcmp(line, "crypto-negative") == 0)) {
        (void)crypto_run_protected_roundtrip();
        return 1;
    }
    if (crypto_is_blocked_command(line)) {
        tm_printf((UB *)"[crypto] BLOCKED: no supported Protected Mode plaintext import and no safe target-managed NVM\n");
        tm_printf((UB *)"ACTION REQUIRED: select a Renesas-supported protected provisioning path and reserve documented nonvolatile storage; no write was performed\n");
        return 1;
    }
#else
    (void)line;
#endif
    return 0;
}
