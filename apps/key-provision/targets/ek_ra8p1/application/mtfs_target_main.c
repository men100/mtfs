/* Dedicated trusted-UART raw key -> HUK-wrapped key provisioner. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <mtkernel/lib/libtm/libtm.h>

#include "hal_data.h"
#include "mbedtls/platform.h"
#include "psa/crypto.h"
#include "r_rsip_key_injection.h"
#include "mtfs_ra8p1_ospi_key_store.h"

EXPORT INT usermain(void);

#define RAW_KEY_BYTES             (32U)
#define XMODEM_128_BLOCK_BYTES    (128U)
#define XMODEM_1K_BLOCK_BYTES     (1024U)
#define XMODEM_MAX_BLOCK_BYTES    XMODEM_1K_BLOCK_BYTES
#define XMODEM_C_INTERVAL_MS (1000U)
#define XMODEM_START_TIMEOUT_MS (60000U)
#define LINE_BYTES          (64U)
#define TAG_BYTES           (16U)
#define TEST_BYTES          (37U)
#define ALIGN               __attribute__((aligned(16)))

#define X_SOH  (0x01)
#define X_STX  (0x02)
#define X_EOT  (0x04)
#define X_ACK  (0x06)
#define X_NAK  (0x15)
#define X_CAN  (0x18)
#define X_CRC  ('C')

static mbedtls_platform_context platform_context;
static uint8_t crypto_ready;

extern INT mtfs_ra8p1_tm_try_getchar(void);

static uint16_t crc16(const uint8_t *data, size_t bytes)
{
    uint16_t crc = 0U;
    size_t index;
    unsigned int bit;

    for (index = 0U; index < bytes; ++index) {
        crc ^= (uint16_t)data[index] << 8;
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc & UINT16_C(0x8000)) != 0U
                ? (uint16_t)((crc << 1) ^ UINT16_C(0x1021))
                : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void xmodem_cancel(void)
{
    tm_putchar(X_CAN);
    tm_putchar(X_CAN);
}

static int xmodem_now_ms(uint32_t *now_ms)
{
    SYSTIM time = {0};

    if ((now_ms == NULL) || (tk_get_otm(&time) != E_OK)) {
        return 0;
    }
    *now_ms = time.lo;
    return 1;
}

static void xmodem_drain_command_terminator(void)
{
    while (mtfs_ra8p1_tm_try_getchar() >= 0) {
        /* The sender cannot start before the first CRC request is emitted. */
    }
}

static int xmodem_wait_for_start(void)
{
    uint32_t start_ms;
    uint32_t last_request_ms;

    if (!xmodem_now_ms(&start_ms)) {
        return -2;
    }
    last_request_ms = start_ms;
    tm_putchar(X_CRC);

    for (;;) {
        uint32_t now_ms;
        int character = mtfs_ra8p1_tm_try_getchar();

        if (character >= 0) {
            return character;
        }
        if (!xmodem_now_ms(&now_ms)) {
            return -2;
        }
        if ((uint32_t)(now_ms - start_ms) >= XMODEM_START_TIMEOUT_MS) {
            return -1;
        }
        if ((uint32_t)(now_ms - last_request_ms) >= XMODEM_C_INTERVAL_MS) {
            tm_putchar(X_CRC);
            last_request_ms = now_ms;
        }
    }

    /* Kept for e2 studio's flow analyzer; the loop exits only by return. */
    return -2;
}

static int xmodem_receive_key(uint8_t raw_key[RAW_KEY_BYTES])
{
    uint8_t block[XMODEM_MAX_BLOCK_BYTES] ALIGN;
    uint8_t number;
    uint8_t inverse;
    uint16_t received_crc;
    size_t block_bytes;
    size_t index;
    int character;
    int padding_ok = 1;

    tm_printf((UB *)"[provision] XMODEM-CRC ready: send an exactly 32-byte binary key file now\n");
    tm_printf((UB *)"[provision] plaintext exists only in RAM during this command\n");
    tm_printf((UB *)"[provision] waiting up to 60 seconds; repeated 'C' is the CRC handshake\n");
    xmodem_drain_command_terminator();
    character = xmodem_wait_for_start();
    if (character == -1) {
        xmodem_cancel();
        tm_printf((UB *)"\n[provision] XMODEM FAIL sender did not start within 60 seconds\n");
        return 0;
    }
    if (character == -2) {
        xmodem_cancel();
        tm_printf((UB *)"\n[provision] XMODEM FAIL monotonic clock unavailable\n");
        return 0;
    }
    if (character == X_CAN) {
        tm_printf((UB *)"\n[provision] XMODEM cancelled by sender\n");
        return 0;
    }
    if (character == X_SOH) {
        block_bytes = XMODEM_128_BLOCK_BYTES;
    } else if (character == X_STX) {
        block_bytes = XMODEM_1K_BLOCK_BYTES;
    } else {
        xmodem_cancel();
        tm_printf((UB *)"\n[provision] XMODEM FAIL expected SOH/STX, got=0x%02x\n",
            (UW)(character & 0xff));
        return 0;
    }
    number = (uint8_t)tm_getchar(1);
    inverse = (uint8_t)tm_getchar(1);
    for (index = 0U; index < block_bytes; ++index) {
        block[index] = (uint8_t)tm_getchar(1);
    }
    received_crc = (uint16_t)((uint16_t)(uint8_t)tm_getchar(1) << 8);
    received_crc |= (uint16_t)(uint8_t)tm_getchar(1);
    if ((number != 1U) || ((uint8_t)(number + inverse) != UINT8_MAX) ||
        (received_crc != crc16(block, block_bytes))) {
        tm_putchar(X_NAK);
        xmodem_cancel();
        mtfs_ra8p1_ospi_key_store_zero(block, sizeof(block));
        tm_printf((UB *)"\n[provision] XMODEM FAIL block/CRC error\n");
        return 0;
    }
    for (index = RAW_KEY_BYTES; index < block_bytes; ++index) {
        if ((block[index] != 0x1aU) && (block[index] != 0U)) {
            padding_ok = 0;
        }
    }
    if (!padding_ok) {
        tm_putchar(X_NAK);
        xmodem_cancel();
        mtfs_ra8p1_ospi_key_store_zero(block, sizeof(block));
        tm_printf((UB *)"\n[provision] XMODEM FAIL file must be exactly 32 bytes\n");
        return 0;
    }
    memcpy(raw_key, block, RAW_KEY_BYTES);
    mtfs_ra8p1_ospi_key_store_zero(block, sizeof(block));
    tm_putchar(X_ACK);
    character = tm_getchar(1);
    if (character != X_EOT) {
        xmodem_cancel();
        mtfs_ra8p1_ospi_key_store_zero(raw_key, RAW_KEY_BYTES);
        tm_printf((UB *)"\n[provision] XMODEM FAIL extra data; expected EOT\n");
        return 0;
    }
    tm_putchar(X_ACK);
    tm_printf((UB *)"\n[provision] XMODEM receive PASS bytes=32 block=%u\n",
        (UW)block_bytes);
    return 1;
}

static psa_status_t crypto_initialize(void)
{
    psa_status_t status;

    if (crypto_ready != 0U) {
        return PSA_SUCCESS;
    }
    if (mbedtls_platform_setup(&platform_context) != 0) {
        return PSA_ERROR_HARDWARE_FAILURE;
    }
    status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        mbedtls_platform_teardown(&platform_context);
        return status;
    }
    crypto_ready = 1U;
    return PSA_SUCCESS;
}

static psa_status_t import_wrapped_key(
    const rsip_aes_wrapped_key_t *wrapped_key,
    psa_key_handle_t *key_handle)
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
        MTFS_RA8P1_AES256_WRAPPED_BYTES, key_handle);
    psa_reset_key_attributes(&attributes);
    return status;
}

static int authentication_rejected(psa_status_t status)
{
    /*
     * FSP 6.5.0 gcm_alt_process.c maps
     * FSP_ERR_CRYPTO_SCE_AUTHENTICATION to
     * MBEDTLS_ERR_PLATFORM_HW_ACCEL_FAILED, which PSA exposes as
     * PSA_ERROR_HARDWARE_FAILURE.  Accept that value only at the deliberate
     * negative-decrypt step, after the positive decrypt above has passed.
     */
    return (status == PSA_ERROR_INVALID_SIGNATURE) ||
        (status == PSA_ERROR_HARDWARE_FAILURE);
}

static int validate_wrapped_key(const rsip_aes_wrapped_key_t *wrapped_key,
    psa_status_t *last_status, const char **last_stage)
{
    static const uint8_t nonce[12] = {
        0x4dU, 0x54U, 0x46U, 0x53U, 0x2dU, 0x50U,
        0x52U, 0x4fU, 0x56U, 0x2dU, 0x30U, 0x32U
    };
    static const uint8_t aad[16] = {
        'M', 'T', 'F', 'S', '-', 'P', 'R', 'O',
        'V', 'I', 'S', 'I', 'O', 'N', '-', '2'
    };
    uint8_t plain[TEST_BYTES] ALIGN;
    uint8_t recovered[TEST_BYTES] ALIGN;
    uint8_t cipher[TEST_BYTES + TAG_BYTES] ALIGN;
    uint8_t damaged[TEST_BYTES + TAG_BYTES] ALIGN;
    psa_key_handle_t handle = 0;
    psa_status_t status;
    size_t cipher_bytes = 0U;
    size_t plain_bytes = 0U;
    size_t index;
    const char *stage = "import";
    int valid = 0;

    for (index = 0U; index < sizeof(plain); ++index) {
        plain[index] = (uint8_t)(index * 9U + 5U);
    }
    status = import_wrapped_key(wrapped_key, &handle);
    if (status == PSA_SUCCESS) {
        stage = "encrypt";
        status = psa_aead_encrypt(handle, PSA_ALG_GCM,
            nonce, sizeof(nonce), aad, sizeof(aad), plain, sizeof(plain),
            cipher, sizeof(cipher), &cipher_bytes);
    }
    if (status == PSA_SUCCESS) {
        stage = "decrypt";
        status = psa_aead_decrypt(handle, PSA_ALG_GCM,
            nonce, sizeof(nonce), aad, sizeof(aad), cipher, cipher_bytes,
            recovered, sizeof(recovered), &plain_bytes);
    }
    if ((status == PSA_SUCCESS) && (plain_bytes == sizeof(plain)) &&
        (memcmp(plain, recovered, sizeof(plain)) == 0)) {
        stage = "negative";
        memcpy(damaged, cipher, cipher_bytes);
        damaged[cipher_bytes - 1U] ^= 1U;
        plain_bytes = 0U;
        status = psa_aead_decrypt(handle, PSA_ALG_GCM,
            nonce, sizeof(nonce), aad, sizeof(aad), damaged, cipher_bytes,
            recovered, sizeof(recovered), &plain_bytes);
        valid = authentication_rejected(status) && (plain_bytes == 0U);
    }
    if (handle != 0U) {
        (void)psa_destroy_key(handle);
    }
    *last_status = status;
    *last_stage = stage;
    mtfs_ra8p1_ospi_key_store_zero(plain, sizeof(plain));
    mtfs_ra8p1_ospi_key_store_zero(recovered, sizeof(recovered));
    mtfs_ra8p1_ospi_key_store_zero(cipher, sizeof(cipher));
    mtfs_ra8p1_ospi_key_store_zero(damaged, sizeof(damaged));
    return valid;
}

static void print_info(void)
{
    tm_printf((UB *)"[provision] mode=RSIP-E50D Compatibility source=UART/XMODEM raw-bytes=32\n");
    tm_printf((UB *)"[provision] destination=onboard-OSPI base=0x90000000 flash-bytes=%u\n",
        (UW)MTFS_RA8P1_OSPI_FLASH_BYTES);
    tm_printf((UB *)"[provision] reserved offsets=0x%08x,0x%08x sector-bytes=%u key-id=%u\n",
        (UW)MTFS_RA8P1_OSPI_KEY_OFFSET_A,
        (UW)MTFS_RA8P1_OSPI_KEY_OFFSET_B,
        (UW)MTFS_RA8P1_OSPI_KEY_SECTOR_BYTES,
        (UW)MTFS_RA8P1_FLEET_KEY_ID);
}

static int load_and_verify(mtfs_ra8p1_key_metadata_t *metadata)
{
    rsip_aes_wrapped_key_t wrapped_key;
    mtfs_ra8p1_key_store_diagnostics_t diagnostics = {0};
    mtfs_ra8p1_key_store_status_t store_status;
    psa_status_t psa_status = PSA_SUCCESS;
    const char *psa_stage = "not-run";
    int valid = 0;

    memset(&wrapped_key, 0, sizeof(wrapped_key));
    store_status = mtfs_ra8p1_ospi_key_store_load(&wrapped_key,
        metadata, &diagnostics);
    if ((store_status == MTFS_RA8P1_KEY_STORE_OK) &&
        validate_wrapped_key(&wrapped_key, &psa_status, &psa_stage)) {
        valid = 1;
        tm_printf((UB *)"[provision] OSPI verify PASS generation=%u key-id=%u key-version=%u slot=0x%08x valid-slots=%u\n",
            (UW)metadata->generation, (UW)metadata->key_id,
            (UW)metadata->key_version, (UW)metadata->slot_offset,
            (UW)diagnostics.valid_slots);
    } else {
        tm_printf((UB *)"[provision] OSPI verify FAIL store=%s fsp=%d psa-stage=%s psa=%d valid-slots=%u\n",
            (UB *)mtfs_ra8p1_key_store_status_string(store_status),
            (INT)diagnostics.last_fsp_error, (UB *)psa_stage,
            (INT)psa_status,
            (UW)diagnostics.valid_slots);
    }
    mtfs_ra8p1_ospi_key_store_zero(&wrapped_key, sizeof(wrapped_key));
    return valid;
}

static void provision_xmodem(int allow_update)
{
    uint8_t raw_key[RAW_KEY_BYTES] ALIGN;
    rsip_aes_wrapped_key_t wrapped_key;
    mtfs_ra8p1_key_metadata_t current = {0U};
    mtfs_ra8p1_key_metadata_t committed = {0U};
    mtfs_ra8p1_key_store_diagnostics_t diagnostics = {0};
    mtfs_ra8p1_key_store_status_t store_status;
    psa_status_t psa_status = PSA_SUCCESS;
    const char *psa_stage = "not-run";
    fsp_err_t fsp_status;
    uint32_t key_version = 1U;

    memset(raw_key, 0, sizeof(raw_key));
    memset(&wrapped_key, 0, sizeof(wrapped_key));
    store_status = mtfs_ra8p1_ospi_key_store_load(&wrapped_key,
        &current, &diagnostics);
    mtfs_ra8p1_ospi_key_store_zero(&wrapped_key, sizeof(wrapped_key));
    if ((store_status == MTFS_RA8P1_KEY_STORE_OK) && !allow_update) {
        tm_printf((UB *)"[provision] BLOCKED: key already exists; use update-xmodem for intentional replacement\n");
        return;
    }
    if (store_status == MTFS_RA8P1_KEY_STORE_OK) {
        key_version = current.key_version + 1U;
        if (key_version == 0U) {
            tm_printf((UB *)"[provision] BLOCKED: key-version exhausted\n");
            return;
        }
    } else if (store_status != MTFS_RA8P1_KEY_STORE_NOT_FOUND) {
        tm_printf((UB *)"[provision] BLOCKED: OSPI unavailable store=%s fsp=%d\n",
            (UB *)mtfs_ra8p1_key_store_status_string(store_status),
            (INT)diagnostics.last_fsp_error);
        return;
    }
    if (!xmodem_receive_key(raw_key)) {
        return;
    }
    fsp_status = R_RSIP_AES256_InitialKeyWrap(
        RSIP_KEY_INJECTION_TYPE_PLAIN, NULL, NULL, raw_key, &wrapped_key);
    mtfs_ra8p1_ospi_key_store_zero(raw_key, sizeof(raw_key));
    if (fsp_status != FSP_SUCCESS) {
        mtfs_ra8p1_ospi_key_store_zero(&wrapped_key, sizeof(wrapped_key));
        tm_printf((UB *)"[provision] FAIL HUK wrap fsp=%d; OSPI unchanged\n",
            fsp_status);
        return;
    }
    tm_printf((UB *)"[provision] plaintext zeroized; validating device-bound wrapped key\n");
    if (!validate_wrapped_key(&wrapped_key, &psa_status, &psa_stage)) {
        mtfs_ra8p1_ospi_key_store_zero(&wrapped_key, sizeof(wrapped_key));
        tm_printf((UB *)"[provision] FAIL wrapped-key GCM positive/negative stage=%s psa=%d; OSPI unchanged\n",
            (UB *)psa_stage, (INT)psa_status);
        return;
    }
    if (psa_status == PSA_ERROR_HARDWARE_FAILURE) {
        tm_printf((UB *)"[provision] negative authentication rejection accepted via FSP 6.5 mapping psa=%d\n",
            (INT)psa_status);
    }
    memset(&diagnostics, 0, sizeof(diagnostics));
    store_status = mtfs_ra8p1_ospi_key_store_commit(&wrapped_key,
        MTFS_RA8P1_FLEET_KEY_ID, key_version, allow_update,
        &committed, &diagnostics);
    mtfs_ra8p1_ospi_key_store_zero(&wrapped_key, sizeof(wrapped_key));
    if (store_status != MTFS_RA8P1_KEY_STORE_OK) {
        tm_printf((UB *)"[provision] FAIL OSPI commit store=%s fsp=%d erase=%u write=%u verify=%u\n",
            (UB *)mtfs_ra8p1_key_store_status_string(store_status),
            (INT)diagnostics.last_fsp_error,
            (UW)diagnostics.erase_count, (UW)diagnostics.write_count,
            (UW)diagnostics.verify_count);
        return;
    }
    tm_printf((UB *)"[provision] OSPI commit PASS generation=%u key-id=%u key-version=%u slot=0x%08x erase=%u write=%u verify=%u\n",
        (UW)committed.generation, (UW)committed.key_id,
        (UW)committed.key_version, (UW)committed.slot_offset,
        (UW)diagnostics.erase_count, (UW)diagnostics.write_count,
        (UW)diagnostics.verify_count);
    if (!load_and_verify(&current)) {
        tm_printf((UB *)"[provision] CRITICAL: committed record failed crypto readback validation\n");
    }
}

static void print_help(void)
{
    tm_printf((UB *)"info              show public provisioning configuration\n");
    tm_printf((UB *)"verify-ospi       validate the active wrapped key; no write\n");
    tm_printf((UB *)"provision-xmodem  create the first key from a 32-byte file\n");
    tm_printf((UB *)"update-xmodem     intentionally replace the key; version increments\n");
    tm_printf((UB *)"help              show this help\n");
}

static void dispatch(const char *line)
{
    mtfs_ra8p1_key_metadata_t metadata = {0U};

    if (strcmp(line, "info") == 0) {
        print_info();
    } else if (strcmp(line, "verify-ospi") == 0) {
        (void)load_and_verify(&metadata);
    } else if (strcmp(line, "provision-xmodem") == 0) {
        provision_xmodem(0);
    } else if (strcmp(line, "update-xmodem") == 0) {
        provision_xmodem(1);
    } else if (strcmp(line, "help") == 0) {
        print_help();
    } else if (line[0] != '\0') {
        tm_printf((UB *)"unknown command; type help\n");
    }
}

static void console_task(INT start_code, void *context)
{
    char line[LINE_BYTES];
    size_t length = 0U;
    int previous_cr = 0;
    psa_status_t status;
    (void)start_code;
    (void)context;

    tm_printf((UB *)"\nmicroT-FS EK-RA8P1 dedicated fleet-key provisioner\n");
    tm_printf((UB *)"Trusted local UART provisioning; plaintext key is transient\n");
    status = crypto_initialize();
    if (status != PSA_SUCCESS) {
        tm_printf((UB *)"[provision] BLOCKED: Compatibility crypto init psa=%d\n",
            (INT)status);
    }
    print_info();
    print_help();
    tm_printf((UB *)"> ");
    for (;;) {
        int character = tm_getchar(1);
        if ((character == '\r') || (character == '\n')) {
            if ((character == '\n') && previous_cr) {
                previous_cr = 0;
                continue;
            }
            previous_cr = character == '\r';
            line[length] = '\0';
            tm_printf((UB *)"\n");
            if (crypto_ready != 0U) {
                dispatch(line);
            } else if (line[0] != '\0') {
                tm_printf((UB *)"[provision] BLOCKED: crypto initialization failed\n");
            }
            length = 0U;
            tm_printf((UB *)"> ");
        } else if ((character == '\b') || (character == 0x7f)) {
            previous_cr = 0;
            if (length != 0U) {
                --length;
                tm_printf((UB *)"\b \b");
            }
        } else if ((character >= 0x20) && (character <= 0x7e)) {
            previous_cr = 0;
            if (length + 1U < sizeof(line)) {
                line[length++] = (char)character;
                tm_putchar(character);
            }
        }
    }
}

EXPORT INT usermain(void)
{
    T_CTSK task = {
        .tskatr = TA_HLNG | TA_RNG3,
        .task = console_task,
        .itskpri = 9,
        .stksz = 24U * 1024U
    };
    ID task_id = tk_cre_tsk(&task);

    if ((task_id <= 0) || (tk_sta_tsk(task_id, 0) < E_OK)) {
        tm_printf((UB *)"[provision] console task start FAIL\n");
    }
    for (;;) {
        (void)tk_slp_tsk(TMO_FEVR);
    }
    return 0;
}
