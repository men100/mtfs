/*
 * CONTEST PROVISIONING TOOL - NOT FOR PRODUCTION.
 *
 * Dedicated firmware that copies an RFP-injected, HUK-wrapped AES-256 key
 * from a temporary MRAM address to an SD card.  It never handles a raw key.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <mtkernel/lib/libtm/libtm.h>

#include "bsp_pin_cfg.h"
#include "ff.h"
#include "hal_data.h"
#include "mtfs_block_device.h"
#include "mtfs_block_registry.h"
#include "mtfs_ra_rsip_key_file.h"
#include "mtfs_ra_sd_spi.h"
#include "mtfs_wrapped_key_fatfs.h"

EXPORT INT usermain(void);

#ifndef MTFS_RA8P1_PROVISION_KEY_ADDRESS
#define MTFS_RA8P1_PROVISION_KEY_ADDRESS (UINT32_C(0))
#endif

#ifndef MTFS_RA8P1_PROVISION_KEY_ID
#define MTFS_RA8P1_PROVISION_KEY_ID (UINT32_C(1))
#endif

#ifndef MTFS_RA8P1_PROVISION_KEY_VERSION
#define MTFS_RA8P1_PROVISION_KEY_VERSION (UINT32_C(1))
#endif

#define PROVISION_KEY_PATH      MTFS_RA_RSIP_FLEET_KEY_PATH
#define PROVISION_TEMP_PATH     "0:/MTFSKEY.TMP"
#define PROVISION_BLOB_BYTES    MTFS_WRAPPED_KEY_RECORD_RSIP_AES256_BLOB_BYTES
#define PROVISION_RECORD_BYTES  MTFS_WRAPPED_KEY_RECORD_RSIP_AES256_BYTES
#define PROVISION_TAG_BYTES     (16U)
#define PROVISION_TEXT_BYTES    (37U)
#define PROVISION_LINE_BYTES    (64U)
#define PROVISION_ALIGN         __attribute__((aligned(16)))

typedef struct provision_storage
{
    mtfs_ra_sd_spi_context_t sd;
    FATFS filesystem;
    mtfs_block_device_t *device;
    uint8_t context_ready;
    uint8_t registered;
    uint8_t mounted;
} provision_storage_t;

static uint8_t record_work[PROVISION_RECORD_BYTES] PROVISION_ALIGN;
static uint8_t crypto_ciphertext[PROVISION_TEXT_BYTES + PROVISION_TAG_BYTES]
    PROVISION_ALIGN;
static uint8_t crypto_plaintext[PROVISION_TEXT_BYTES + PROVISION_TAG_BYTES]
    PROVISION_ALIGN;
static uint8_t crypto_output[PROVISION_TEXT_BYTES + PROVISION_TAG_BYTES]
    PROVISION_ALIGN;

static void secure_zero(void *data, size_t data_bytes)
{
    volatile uint8_t *cursor = (volatile uint8_t *)data;

    while (data_bytes != 0U) {
        *cursor++ = 0U;
        --data_bytes;
    }
    __asm volatile ("" : : "r" (data) : "memory");
}

static int card_present(void *context)
{
    bsp_io_level_t level = BSP_IO_LEVEL_HIGH;
    (void)context;

    if (g_ioport.p_api->pinRead(g_ioport.p_ctrl,
            PMOD2_GPIO1, &level) != FSP_SUCCESS) {
        return 0;
    }
    return level == BSP_IO_LEVEL_LOW;
}

static void storage_config(mtfs_ra_sd_spi_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->device_name = "hspia";
    config->spi = &g_sci_spi0;
    config->ioport = &g_ioport;
    config->chip_select_pin = PMOD2_CTS;
    config->initialization_bitrate_hz = 400000U;
    config->data_bitrate_hz = 4000000U;
    config->initialization_timeout_ms = 1000U;
    config->transfer_timeout_ms = 1000U;
    config->card_present = card_present;
}

static mtfs_error_t storage_open(provision_storage_t *storage)
{
    mtfs_ra_sd_spi_config_t config;
    mtfs_error_t error;

    memset(storage, 0, sizeof(*storage));
    storage_config(&config);
    error = mtfs_ra_sd_spi_context_init(&storage->sd, &config);
    if (error != MTFS_OK) {
        return error;
    }
    storage->context_ready = 1U;
    storage->device = mtfs_ra_sd_spi_block_device(&storage->sd);
    error = mtfs_block_initialize(storage->device);
    if (error != MTFS_OK) {
        return error;
    }
    error = mtfs_block_registry_register(0U, storage->device);
    if (error != MTFS_OK) {
        return error;
    }
    storage->registered = 1U;
    if (f_mount(&storage->filesystem, "0:", 1U) != FR_OK) {
        return MTFS_ERROR_IO;
    }
    storage->mounted = 1U;
    return MTFS_OK;
}

static void storage_close(provision_storage_t *storage)
{
    if (storage->mounted) {
        (void)f_mount(NULL, "0:", 0U);
        storage->mounted = 0U;
    }
    if (storage->registered) {
        (void)mtfs_block_registry_unregister(0U);
        storage->registered = 0U;
    }
    if (storage->context_ready) {
        (void)mtfs_ra_sd_spi_context_deinit(&storage->sd);
        storage->context_ready = 0U;
    }
}

static int injected_blob_present(const uint8_t *blob)
{
    uint8_t all_zero = 0U;
    uint8_t all_erased = 0xffU;
    size_t index;

    for (index = 0U; index < PROVISION_BLOB_BYTES; ++index) {
        all_zero |= blob[index];
        all_erased &= blob[index];
    }
    return (all_zero != 0U) && (all_erased != 0xffU);
}

static fsp_err_t crypto_decrypt(
    const rsip_wrapped_key_t *key,
    const uint8_t *nonce,
    const uint8_t *aad,
    uint32_t aad_bytes,
    const uint8_t *ciphertext,
    uint32_t ciphertext_bytes,
    const uint8_t *tag,
    uint8_t *output)
{
    uint32_t update_bytes = 0U;
    uint32_t finish_bytes = 0U;
    fsp_err_t error = R_RSIP_AES_AEAD_Init(g_rsip.p_ctrl,
        RSIP_AES_AEAD_MODE_GCM_DEC, key, nonce, 12U);

    if (error == FSP_SUCCESS) {
        error = R_RSIP_AES_AEAD_AADUpdate(g_rsip.p_ctrl,
            aad, aad_bytes);
    }
    if (error == FSP_SUCCESS) {
        error = R_RSIP_AES_AEAD_Update(g_rsip.p_ctrl,
            ciphertext, ciphertext_bytes, output, &update_bytes);
    }
    if (error == FSP_SUCCESS) {
        error = R_RSIP_AES_AEAD_Verify(g_rsip.p_ctrl,
            output + update_bytes, &finish_bytes, tag,
            PROVISION_TAG_BYTES);
    }
    if ((error == FSP_SUCCESS) &&
        ((update_bytes + finish_bytes) != ciphertext_bytes)) {
        return FSP_ERR_CRYPTO_UNKNOWN;
    }
    return error;
}

static int crypto_validate_blob(const uint8_t *blob, fsp_err_t *last_error)
{
    static const uint8_t nonce[12] PROVISION_ALIGN = {
        0x4dU, 0x54U, 0x46U, 0x53U, 0x2dU, 0x50U,
        0x52U, 0x4fU, 0x56U, 0x2dU, 0x30U, 0x31U
    };
    static const uint8_t aad[16] PROVISION_ALIGN = {
        'M', 'T', 'F', 'S', '-', 'P', 'R', 'O',
        'V', 'I', 'S', 'I', 'O', 'N', '-', '1'
    };
    uint8_t tag[PROVISION_TAG_BYTES] PROVISION_ALIGN = {0U};
    uint8_t bad_tag[PROVISION_TAG_BYTES] PROVISION_ALIGN = {0U};
    uint32_t update_bytes = 0U;
    uint32_t finish_bytes = 0U;
    rsip_wrapped_key_t key = {RSIP_KEY_TYPE_AES_256, (void *)blob};
    fsp_err_t error;
    size_t index;
    int opened = 0;
    int valid = 0;

    for (index = 0U; index < PROVISION_TEXT_BYTES; ++index) {
        crypto_plaintext[index] = (uint8_t)(index * 9U + 5U);
    }
    secure_zero(crypto_ciphertext, sizeof(crypto_ciphertext));
    secure_zero(crypto_output, sizeof(crypto_output));
    error = R_RSIP_Open(g_rsip.p_ctrl, g_rsip.p_cfg);
    if (error == FSP_SUCCESS) {
        opened = 1;
        error = R_RSIP_AES_AEAD_Init(g_rsip.p_ctrl,
            RSIP_AES_AEAD_MODE_GCM_ENC, &key, nonce, sizeof(nonce));
    }
    if (error == FSP_SUCCESS) {
        error = R_RSIP_AES_AEAD_AADUpdate(g_rsip.p_ctrl,
            aad, sizeof(aad));
    }
    if (error == FSP_SUCCESS) {
        error = R_RSIP_AES_AEAD_Update(g_rsip.p_ctrl,
            crypto_plaintext, PROVISION_TEXT_BYTES,
            crypto_ciphertext, &update_bytes);
    }
    if (error == FSP_SUCCESS) {
        error = R_RSIP_AES_AEAD_Finish(g_rsip.p_ctrl,
            crypto_ciphertext + update_bytes, &finish_bytes, tag);
    }
    if ((error == FSP_SUCCESS) &&
        ((update_bytes + finish_bytes) == PROVISION_TEXT_BYTES)) {
        error = crypto_decrypt(&key, nonce, aad, sizeof(aad),
            crypto_ciphertext, PROVISION_TEXT_BYTES, tag, crypto_output);
    }
    if ((error == FSP_SUCCESS) &&
        (memcmp(crypto_output, crypto_plaintext,
            PROVISION_TEXT_BYTES) == 0)) {
        memcpy(bad_tag, tag, sizeof(bad_tag));
        bad_tag[0] ^= 1U;
        secure_zero(crypto_output, sizeof(crypto_output));
        error = crypto_decrypt(&key, nonce, aad, sizeof(aad),
            crypto_ciphertext, PROVISION_TEXT_BYTES,
            bad_tag, crypto_output);
        if (error == FSP_ERR_CRYPTO_RSIP_AUTHENTICATION) {
            valid = 1;
        }
    }
    *last_error = error;
    if (opened) {
        (void)R_RSIP_Close(g_rsip.p_ctrl);
    }
    secure_zero(tag, sizeof(tag));
    secure_zero(bad_tag, sizeof(bad_tag));
    secure_zero(crypto_plaintext, sizeof(crypto_plaintext));
    secure_zero(crypto_ciphertext, sizeof(crypto_ciphertext));
    secure_zero(crypto_output, sizeof(crypto_output));
    return valid;
}

static const uint8_t *injected_blob(void)
{
    if (MTFS_RA8P1_PROVISION_KEY_ADDRESS == 0U) {
        return NULL;
    }
    return (const uint8_t *)(uintptr_t)MTFS_RA8P1_PROVISION_KEY_ADDRESS;
}

static void print_info(void)
{
    tm_printf((UB *)"[provision] CONTEST TOOL - NOT FOR PRODUCTION\n");
    tm_printf((UB *)"[provision] source=RFP-injected MRAM address=0x%08x bytes=%u configured=%s\n",
        (UW)MTFS_RA8P1_PROVISION_KEY_ADDRESS,
        (UW)PROVISION_BLOB_BYTES,
        MTFS_RA8P1_PROVISION_KEY_ADDRESS != 0U
            ? (UB *)"yes" : (UB *)"no");
    tm_printf((UB *)"[provision] destination=%s temporary=%s key_id=%u key_version=%u overwrite=no format=no\n",
        (UB *)PROVISION_KEY_PATH, (UB *)PROVISION_TEMP_PATH,
        (UW)MTFS_RA8P1_PROVISION_KEY_ID,
        (UW)MTFS_RA8P1_PROVISION_KEY_VERSION);
}

static int verify_injected(void)
{
    const uint8_t *blob = injected_blob();
    fsp_err_t error = FSP_SUCCESS;

    if (blob == NULL) {
        tm_printf((UB *)"[provision] BLOCKED: MTFS_RA8P1_PROVISION_KEY_ADDRESS is not configured\n");
        return 0;
    }
    if (((uintptr_t)blob & 15U) != 0U) {
        tm_printf((UB *)"[provision] BLOCKED: injected wrapped-key address is not 16-byte aligned\n");
        return 0;
    }
    if (!injected_blob_present(blob)) {
        tm_printf((UB *)"[provision] FAIL: injection area is zero/erased; no SD write performed\n");
        return 0;
    }
    if (!crypto_validate_blob(blob, &error)) {
        tm_printf((UB *)"[provision] FAIL: injected wrapped key rejected by RSIP fsp=%d; no SD write performed\n",
            error);
        return 0;
    }
    tm_printf((UB *)"[provision] injected wrapped-key GCM positive/negative PASS\n");
    return 1;
}

static void verify_sd(void)
{
    provision_storage_t storage;
    mtfs_ra_rsip_key_file_t key_file;
    mtfs_error_t error;
    fsp_err_t fsp_error = FSP_SUCCESS;

    mtfs_ra_rsip_key_file_init(&key_file);
    error = storage_open(&storage);
    if (error == MTFS_OK) {
        error = mtfs_ra_rsip_key_file_load(&key_file,
            PROVISION_KEY_PATH);
    }
    if ((error == MTFS_OK) &&
        crypto_validate_blob(
            (const uint8_t *)mtfs_ra_rsip_key_file_key(&key_file)->p_value,
            &fsp_error)) {
        tm_printf((UB *)"[provision] SD wrapped-key key_id=%u key_version=%u RSIP PASS\n",
            (UW)key_file.metadata.key_id,
            (UW)key_file.metadata.key_version);
    } else {
        tm_printf((UB *)"[provision] SD verify FAIL mtfs=%d storage=%s fatfs=%u record=%s fsp=%d\n",
            error,
            (UB *)mtfs_wrapped_key_fatfs_status_string(
                key_file.last_load_status),
            (UW)key_file.diagnostics.last_fatfs_result,
            (UB *)mtfs_wrapped_key_record_status_string(
                key_file.diagnostics.last_record_status),
            fsp_error);
    }
    mtfs_ra_rsip_key_file_unload(&key_file);
    storage_close(&storage);
}

static void provision_sd(void)
{
    provision_storage_t storage;
    mtfs_ra_rsip_key_file_t key_file;
    mtfs_wrapped_key_metadata_t metadata = {
        MTFS_WRAPPED_KEY_PROVIDER_RA_RSIP_E50D,
        MTFS_WRAPPED_KEY_TYPE_AES_256,
        MTFS_RA8P1_PROVISION_KEY_ID,
        MTFS_RA8P1_PROVISION_KEY_VERSION
    };
    mtfs_wrapped_key_fatfs_diagnostics_t diagnostics = {0};
    const uint8_t *blob = injected_blob();
    mtfs_wrapped_key_fatfs_status_t status;
    mtfs_error_t error;
    fsp_err_t fsp_error = FSP_SUCCESS;

    if (!verify_injected()) {
        return;
    }
    mtfs_ra_rsip_key_file_init(&key_file);
    error = storage_open(&storage);
    if (error != MTFS_OK) {
        tm_printf((UB *)"[provision] SD setup FAIL mtfs=%d\n", error);
        storage_close(&storage);
        return;
    }
    status = mtfs_wrapped_key_fatfs_create(PROVISION_KEY_PATH,
        PROVISION_TEMP_PATH, &metadata, blob, PROVISION_BLOB_BYTES,
        record_work, sizeof(record_work), &diagnostics);
    if (status != MTFS_WRAPPED_KEY_FATFS_OK) {
        tm_printf((UB *)"[provision] SD create FAIL status=%s fatfs=%u record=%s; existing files were not changed\n",
            (UB *)mtfs_wrapped_key_fatfs_status_string(status),
            (UW)diagnostics.last_fatfs_result,
            (UB *)mtfs_wrapped_key_record_status_string(
                diagnostics.last_record_status));
        storage_close(&storage);
        return;
    }
    error = mtfs_ra_rsip_key_file_load(&key_file, PROVISION_KEY_PATH);
    if ((error != MTFS_OK) ||
        !crypto_validate_blob(
            (const uint8_t *)mtfs_ra_rsip_key_file_key(&key_file)->p_value,
            &fsp_error)) {
        tm_printf((UB *)"[provision] FAIL after commit: keep SD for inspection mtfs=%d fsp=%d\n",
            error, fsp_error);
    } else {
        tm_printf((UB *)"[provision] PASS destination=%s key_id=%u key_version=%u writes=%u sync=%u readback=%u\n",
            (UB *)PROVISION_KEY_PATH,
            (UW)key_file.metadata.key_id,
            (UW)key_file.metadata.key_version,
            (UW)diagnostics.create_count,
            (UW)diagnostics.sync_count,
            (UW)diagnostics.verify_count);
    }
    mtfs_ra_rsip_key_file_unload(&key_file);
    storage_close(&storage);
}

static void print_help(void)
{
    tm_printf((UB *)"info             show public provisioning configuration\n");
    tm_printf((UB *)"verify-injected  validate the RFP-injected wrapped key; no write\n");
    tm_printf((UB *)"verify-sd        validate an existing MTFSKEY.BIN; no write\n");
    tm_printf((UB *)"provision-sd     create MTFSKEY.BIN once; never overwrite or format\n");
    tm_printf((UB *)"help             show this help\n");
}

static void dispatch(const char *line)
{
    if (strcmp(line, "info") == 0) {
        print_info();
    } else if (strcmp(line, "verify-injected") == 0) {
        (void)verify_injected();
    } else if (strcmp(line, "verify-sd") == 0) {
        verify_sd();
    } else if (strcmp(line, "provision-sd") == 0) {
        provision_sd();
    } else if (strcmp(line, "help") == 0) {
        print_help();
    } else if (line[0] != '\0') {
        tm_printf((UB *)"unknown command; type help\n");
    }
}

static void console_task(INT start_code, void *context)
{
    char line[PROVISION_LINE_BYTES];
    size_t length = 0U;
    int previous_cr = 0;
    (void)start_code;
    (void)context;

    tm_printf((UB *)"\nmicroT-FS EK-RA8P1 dedicated wrapped-key provisioner\n");
    tm_printf((UB *)"CONTEST TOOL - NOT FOR PRODUCTION; no raw key input\n");
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
            dispatch(line);
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
        .stksz = 16U * 1024U
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
