/* Dedicated trusted-UART raw key -> STM32N657 DHUK-wrapped key provisioner. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app/mtfs_app_log.h"

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "mtfs_stm32_saes.h"
#include "mtfs_stm32_nor_key_store.h"
#include "mtfs_stm32n6570_nor.h"
#include "app/mtfs_key_provision_command.h"

#define RAW_KEY_BYTES          (32U)
#define XMODEM_128_BYTES       (128U)
#define XMODEM_1K_BYTES        (1024U)
#define XMODEM_START_MS        (60000U)
#define XMODEM_C_MS            (1000U)
#define XMODEM_BYTE_MS         (10000U)
#define LINE_BYTES             (64U)
#define X_SOH                  (0x01)
#define X_STX                  (0x02)
#define X_EOT                  (0x04)
#define X_ACK                  (0x06)
#define X_NAK                  (0x15)
#define X_CAN                  (0x18)
#define X_CRC                  ('C')

#define SECURE_USART1_ISR (*(volatile uint32_t *)(uintptr_t)UINT32_C(0x5200101c))
#define SECURE_USART1_RDR (*(volatile uint32_t *)(uintptr_t)UINT32_C(0x52001024))
#define PROVISION_USART_RXNE (UINT32_C(0x20))

static mtfs_stm32_saes_context_t crypto_context;

static int try_getchar(void)
{
    if ((SECURE_USART1_ISR & PROVISION_USART_RXNE) == 0U) return -1;
    return (int)(SECURE_USART1_RDR & 0xffU);
}

static uint16_t crc16(const uint8_t *data, size_t bytes)
{
    uint16_t crc = 0U;
    size_t index;
    for (index = 0U; index < bytes; ++index) {
        unsigned int bit;
        crc ^= (uint16_t)data[index] << 8;
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc & UINT16_C(0x8000)) != 0U ?
                (uint16_t)((crc << 1) ^ UINT16_C(0x1021)) :
                (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void xmodem_cancel(void)
{
    tm_putchar(X_CAN);
    tm_putchar(X_CAN);
}

static int now_ms(uint32_t *value)
{
    SYSTIM time = {0};
    if ((value == NULL) || (tk_get_otm(&time) != E_OK)) return 0;
    *value = time.lo;
    return 1;
}

static int wait_for_xmodem(void)
{
    uint32_t start, last;
    if (!now_ms(&start)) return -2;
    last = start;
    tm_putchar(X_CRC);
    for (;;) {
        uint32_t current;
        int value = try_getchar();
        if (value >= 0) return value;
        if (!now_ms(&current)) return -2;
        if ((uint32_t)(current - start) >= XMODEM_START_MS) return -1;
        if ((uint32_t)(current - last) >= XMODEM_C_MS) {
            tm_putchar(X_CRC);
            last = current;
        }
    }
}

static int wait_for_byte(uint32_t timeout_ms)
{
    uint32_t start;
    if (!now_ms(&start)) return -2;
    for (;;) {
        uint32_t current;
        int value = try_getchar();
        if (value >= 0) return value;
        if (!now_ms(&current)) return -2;
        if ((uint32_t)(current - start) >= timeout_ms) return -1;
    }
}

static int receive_key(uint8_t raw_key[RAW_KEY_BYTES])
{
    uint8_t block[XMODEM_1K_BYTES] __attribute__((aligned(16)));
    uint8_t number, inverse;
    uint16_t received_crc;
    size_t block_bytes, index;
    int value;
    while (try_getchar() >= 0) { }
    tm_printf((UB *)"[provision] XMODEM-CRC ready: send exactly 32 binary bytes\n");
    tm_printf((UB *)"[provision] plaintext is transient RAM data; timeout=60s\n");
    value = wait_for_xmodem();
    if ((value == -1) || (value == -2)) {
        xmodem_cancel();
        tm_printf((UB *)"\n[provision] XMODEM FAIL start timeout/clock\n");
        return 0;
    }
    if (value == X_SOH) block_bytes = XMODEM_128_BYTES;
    else if (value == X_STX) block_bytes = XMODEM_1K_BYTES;
    else {
        xmodem_cancel();
        tm_printf((UB *)"\n[provision] XMODEM FAIL expected SOH/STX got=0x%02x\n",
            (uint32_t)(value & 0xff));
        return 0;
    }
    value = wait_for_byte(XMODEM_BYTE_MS);
    if (value < 0) goto incomplete;
    number = (uint8_t)value;
    value = wait_for_byte(XMODEM_BYTE_MS);
    if (value < 0) goto incomplete;
    inverse = (uint8_t)value;
    for (index = 0U; index < block_bytes; ++index) {
        value = wait_for_byte(XMODEM_BYTE_MS);
        if (value < 0) goto incomplete;
        block[index] = (uint8_t)value;
    }
    value = wait_for_byte(XMODEM_BYTE_MS);
    if (value < 0) goto incomplete;
    received_crc = (uint16_t)((uint16_t)(uint8_t)value << 8);
    value = wait_for_byte(XMODEM_BYTE_MS);
    if (value < 0) goto incomplete;
    received_crc |= (uint16_t)(uint8_t)value;
    if ((number != 1U) || ((uint8_t)(number + inverse) != UINT8_MAX) ||
        (received_crc != crc16(block, block_bytes))) {
        tm_putchar(X_NAK);
        xmodem_cancel();
        mtfs_stm32_saes_zeroize(block, sizeof(block));
        tm_printf((UB *)"\n[provision] XMODEM FAIL block/CRC\n");
        return 0;
    }
    for (index = RAW_KEY_BYTES; index < block_bytes; ++index) {
        if ((block[index] != 0U) && (block[index] != 0x1aU)) {
            tm_putchar(X_NAK);
            xmodem_cancel();
            mtfs_stm32_saes_zeroize(block, sizeof(block));
            tm_printf((UB *)"\n[provision] XMODEM FAIL file must be exactly 32 bytes\n");
            return 0;
        }
    }
    memcpy(raw_key, block, RAW_KEY_BYTES);
    mtfs_stm32_saes_zeroize(block, sizeof(block));
    tm_putchar(X_ACK);
    value = wait_for_byte(XMODEM_BYTE_MS);
    if (value != X_EOT) {
        xmodem_cancel();
        mtfs_stm32_saes_zeroize(raw_key, RAW_KEY_BYTES);
        tm_printf((UB *)"\n[provision] XMODEM FAIL extra/incomplete input\n");
        return 0;
    }
    tm_putchar(X_ACK);
    tm_printf((UB *)"\n[provision] XMODEM receive PASS bytes=32 block=%u\n",
        (uint32_t)block_bytes);
    return 1;
incomplete:
    xmodem_cancel();
    mtfs_stm32_saes_zeroize(block, sizeof(block));
    mtfs_stm32_saes_zeroize(raw_key, RAW_KEY_BYTES);
    tm_printf((UB *)"\n[provision] XMODEM FAIL incomplete input/clock\n");
    return 0;
}

static int all_zero(const uint8_t *data, size_t bytes)
{
    uint8_t combined = 0U;
    size_t index;
    for (index = 0U; index < bytes; ++index) combined |= data[index];
    return combined == 0U;
}

static int validate_key(const mtfs_stm32_saes_wrapped_key_t *key)
{
    static const uint8_t nonce[12] = {
        0x4d,0x54,0x46,0x53,0x50,0x52,0x4f,0x56,0x00,0x00,0x00,0x01
    };
    static const uint8_t aad[] = "MTFS-PROVISION-v1";
    uint8_t plain[37], cipher[37], recovered[37], tag[16];
    mtfs_stm32_saes_status_t status;
    const char *stage = "encrypt";
    size_t index;
    for (index = 0U; index < sizeof(plain); ++index)
        plain[index] = (uint8_t)(index * 9U + 5U);
    status = mtfs_stm32_saes_encrypt_wrapped(&crypto_context, key, nonce,
        aad, sizeof(aad) - 1U, plain, sizeof(plain), cipher, tag);
    if (status != MTFS_STM32_SAES_OK) goto fail;
    stage = "positive-decrypt";
    status = mtfs_stm32_saes_decrypt_wrapped(&crypto_context, key, nonce,
        aad, sizeof(aad) - 1U, cipher, sizeof(cipher), tag, recovered);
    if (status != MTFS_STM32_SAES_OK) goto fail;
    stage = "positive-compare";
    if (memcmp(plain, recovered, sizeof(plain)) != 0) goto fail;
    tag[0] ^= 1U;
    memset(recovered, 0xa5, sizeof(recovered));
    stage = "negative-decrypt";
    status = mtfs_stm32_saes_decrypt_wrapped(&crypto_context, key, nonce,
        aad, sizeof(aad) - 1U, cipher, sizeof(cipher), tag, recovered);
    if (status != MTFS_STM32_SAES_AUTH_FAILED) goto fail;
    stage = "negative-zeroize";
    if (!all_zero(recovered, sizeof(recovered))) goto fail;
    mtfs_stm32_saes_zeroize(plain, sizeof(plain));
    mtfs_stm32_saes_zeroize(cipher, sizeof(cipher));
    mtfs_stm32_saes_zeroize(recovered, sizeof(recovered));
    mtfs_stm32_saes_zeroize(tag, sizeof(tag));
    return 1;
fail:
    tm_printf((UB *)"[provision] wrapped-key validation FAIL stage=%s status=%s hal-op=%s hal=%u error=0x%08x CR=0x%08x SR=0x%08x ISR=0x%08x\n",
        (UB *)stage, (UB *)mtfs_stm32_saes_status_string(status),
        (UB *)mtfs_stm32_saes_hal_operation_string(
            crypto_context.last_hal_operation),
        crypto_context.last_hal_status, crypto_context.last_hal_error,
        crypto_context.last_saes_cr, crypto_context.last_saes_sr,
        crypto_context.last_saes_isr);
    mtfs_stm32_saes_zeroize(plain, sizeof(plain));
    mtfs_stm32_saes_zeroize(cipher, sizeof(cipher));
    mtfs_stm32_saes_zeroize(recovered, sizeof(recovered));
    mtfs_stm32_saes_zeroize(tag, sizeof(tag));
    return 0;
}

static int open_store(mtfs_stm32_nor_io_t *io)
{
    int32_t error = 0;
    if (mtfs_stm32n6570_nor_open(io, &error) != 0) {
        tm_printf((UB *)"[provision] BLOCKED NOR init bsp=%d\n", error);
        return 0;
    }
    return 1;
}

static int load_and_verify(void)
{
    mtfs_stm32_nor_io_t io;
    mtfs_stm32_nor_wrapped_key_t stored;
    mtfs_stm32_saes_wrapped_key_t key;
    mtfs_stm32_nor_key_metadata_t metadata;
    mtfs_stm32_nor_key_store_diagnostics_t diagnostics = {0};
    mtfs_stm32_nor_key_store_status_t store_status;
    int valid = 0;
    if (!open_store(&io)) return 0;
    store_status = mtfs_stm32_nor_key_store_load(&io, &stored, &metadata,
        &diagnostics);
    if (store_status == MTFS_STM32_NOR_KEY_STORE_OK) {
        memcpy(key.bytes, stored.bytes, sizeof(key.bytes));
        valid = validate_key(&key);
    }
    tm_printf((UB *)"[provision] NOR verify %s store=%s generation=%u key-id=%u key-version=%u slot=0x%08x valid-slots=%u\n",
        valid ? (UB *)"PASS" : (UB *)"FAIL",
        (UB *)mtfs_stm32_nor_key_store_status_string(store_status),
        valid ? metadata.generation : 0U, valid ? metadata.key_id : 0U,
        valid ? metadata.key_version : 0U,
        valid ? metadata.slot_offset : 0U, diagnostics.valid_slots);
    mtfs_stm32_saes_zeroize(&stored, sizeof(stored));
    mtfs_stm32_saes_zeroize(&key, sizeof(key));
    return valid;
}

static void provision(int replace)
{
    mtfs_stm32_nor_io_t io;
    uint8_t raw_key[RAW_KEY_BYTES] __attribute__((aligned(16)));
    mtfs_stm32_saes_wrapped_key_t wrapped;
    mtfs_stm32_nor_wrapped_key_t stored, current_key;
    mtfs_stm32_nor_key_metadata_t current = {0}, committed = {0};
    mtfs_stm32_nor_key_store_diagnostics_t diagnostics = {0};
    mtfs_stm32_nor_key_store_status_t store_status;
    mtfs_stm32_saes_status_t crypto_status;
    const uint32_t key_version = MTFS_STM32_NOR_FLEET_KEY_VERSION;
    memset(raw_key, 0, sizeof(raw_key));
    memset(&wrapped, 0, sizeof(wrapped));
    memset(&stored, 0, sizeof(stored));
    memset(&current_key, 0, sizeof(current_key));
    if (!open_store(&io)) return;
    store_status = mtfs_stm32_nor_key_store_load(&io, &current_key,
        &current, &diagnostics);
    mtfs_stm32_saes_zeroize(&current_key, sizeof(current_key));
    if ((store_status == MTFS_STM32_NOR_KEY_STORE_OK) && !replace) {
        tm_printf((UB *)"[provision] BLOCKED already provisioned; use \"provision-xmodem replace\"\n");
        return;
    }
    if ((store_status == MTFS_STM32_NOR_KEY_STORE_NOT_FOUND) && replace) {
        tm_printf((UB *)"[provision] BLOCKED no key exists; use \"provision-xmodem\" for initial provisioning\n");
        return;
    }
    if (store_status == MTFS_STM32_NOR_KEY_STORE_OK) {
        tm_printf((UB *)"[provision] replacing generation=%u key-id=%u key-version=%u slot=0x%08x\n",
            current.generation, current.key_id, current.key_version,
            current.slot_offset);
        if ((current.key_id != MTFS_STM32_NOR_FLEET_KEY_ID) ||
            (current.key_version != key_version)) {
            tm_printf((UB *)"[provision] BLOCKED active record is not fleet key 1/1\n");
            return;
        }
    } else if (store_status != MTFS_STM32_NOR_KEY_STORE_NOT_FOUND) {
        tm_printf((UB *)"[provision] BLOCKED NOR scan store=%s io=%d\n",
            (UB *)mtfs_stm32_nor_key_store_status_string(store_status),
            diagnostics.last_io_error);
        return;
    }
    if (!receive_key(raw_key)) return;
    crypto_status = mtfs_stm32_saes_wrap_key(&crypto_context, raw_key,
        &wrapped);
    mtfs_stm32_saes_zeroize(raw_key, sizeof(raw_key));
    if ((crypto_status != MTFS_STM32_SAES_OK) ||
        !all_zero(raw_key, sizeof(raw_key))) {
        tm_printf((UB *)"[provision] FAIL DHUK wrap status=%s hal-op=%s hal=%u error=0x%08x CR=0x%08x SR=0x%08x ISR=0x%08x; NOR unchanged\n",
            (UB *)mtfs_stm32_saes_status_string(crypto_status),
            (UB *)mtfs_stm32_saes_hal_operation_string(
                crypto_context.last_hal_operation),
            crypto_context.last_hal_status, crypto_context.last_hal_error,
            crypto_context.last_saes_cr, crypto_context.last_saes_sr,
            crypto_context.last_saes_isr);
        goto cleanup;
    }
    if (!validate_key(&wrapped)) {
        tm_printf((UB *)"[provision] FAIL wrapped-key positive/negative; NOR unchanged\n");
        goto cleanup;
    }
    memcpy(stored.bytes, wrapped.bytes, sizeof(stored.bytes));
    memset(&diagnostics, 0, sizeof(diagnostics));
    store_status = mtfs_stm32_nor_key_store_commit(&io, &stored,
        MTFS_STM32_NOR_FLEET_KEY_ID, key_version, replace,
        &committed, &diagnostics);
    if (store_status != MTFS_STM32_NOR_KEY_STORE_OK) {
        tm_printf((UB *)"[provision] FAIL NOR commit store=%s io=%d erase=%u write=%u verify=%u\n",
            (UB *)mtfs_stm32_nor_key_store_status_string(store_status),
            diagnostics.last_io_error, diagnostics.erase_count,
            diagnostics.write_count, diagnostics.verify_count);
        goto cleanup;
    }
    tm_printf((UB *)"[provision] NOR commit PASS generation=%u key-version=%u slot=0x%08x erase=%u write=%u verify=%u\n",
        committed.generation, committed.key_version, committed.slot_offset,
        diagnostics.erase_count, diagnostics.write_count,
        diagnostics.verify_count);
    if (!load_and_verify())
        tm_printf((UB *)"[provision] CRITICAL readback crypto validation failed\n");
cleanup:
    mtfs_stm32_saes_zeroize(raw_key, sizeof(raw_key));
    mtfs_stm32_saes_zeroize(&wrapped, sizeof(wrapped));
    mtfs_stm32_saes_zeroize(&stored, sizeof(stored));
}

static void info(void)
{
    tm_printf((UB *)"[provision] provider=STM32N657 SAES/DHUK raw=32 wrapped=32 FullSecure\n");
    tm_printf((UB *)"[provision] NOR=MX66UW1G45G bytes=%u erase=%u page=%u slots=0x%08x/0x%08x\n",
        MTFS_STM32_NOR_BYTES, MTFS_STM32_NOR_ERASE_BYTES,
        MTFS_STM32_NOR_PROGRAM_BYTES, MTFS_STM32_NOR_KEY_OFFSET_A,
        MTFS_STM32_NOR_KEY_OFFSET_B);
}

static mtfs_app_log_level_t provision_log_level = MTFS_APP_LOG_INFO;

static void print_log_level(const char *prefix)
{
    tm_printf((UB *)"%s%s\n", (UB *)prefix,
        (UB *)mtfs_app_log_level_name(provision_log_level));
}

static void help_group(const char *group)
{
    int all = strcmp(group, "all") == 0;
    int known = 0;
    if (all || strcmp(group, "general") == 0) {
        known = 1;
        tm_printf((UB *)"General:\n");
        tm_printf((UB *)"  help                         show commands and groups\n");
        tm_printf((UB *)"  help <group>                 show one command group\n");
        tm_printf((UB *)"  help all                     show every command\n");
        tm_printf((UB *)"  log-level [off|error|info|debug]\n");
        tm_printf((UB *)"  info                         public configuration; no write\n");
    }
    if (all || strcmp(group, "provisioning") == 0) {
        known = 1;
        tm_printf((UB *)"Provisioning:\n");
        tm_printf((UB *)"  verify-nor        load and crypto-validate; no write\n");
        tm_printf((UB *)"  provision-xmodem  create first key from exact 32-byte file\n");
        tm_printf((UB *)"  provision-xmodem replace\n");
        tm_printf((UB *)"                       replace the single fleet key; keep ID/version 1/1\n");
    }
    if (!known) {
        tm_printf((UB *)"ERROR: unknown help group\n");
        tm_printf((UB *)"Groups: general provisioning\n");
    }
}

static void help(void)
{
    tm_printf((UB *)"General:\n");
    tm_printf((UB *)"  help                         show commands and groups\n");
    tm_printf((UB *)"  log-level [off|error|info|debug]\n");
    tm_printf((UB *)"  info                         public configuration; no write\n");
    tm_printf((UB *)"Groups:\n  general\n  provisioning\n");
    tm_printf((UB *)"Use help <group> for details.\n");
}

static void dispatch(const char *line)
{
    mtfs_key_provision_command_t provision_command =
        mtfs_key_provision_command_parse(line);
    mtfs_app_log_level_t requested_level = provision_log_level;
    int is_query = 0;
    int log_command = mtfs_app_log_parse_command(
        line, &requested_level, &is_query);

    if (log_command < 0)
        tm_printf((UB *)"ERROR: use log-level off|error|info|debug\n");
    else if (log_command > 0) {
        if (!is_query) provision_log_level = requested_level;
        print_log_level(is_query ? "log-level: " : "log-level set: ");
    }
    else if (strcmp(line, "info") == 0) info();
    else if (strcmp(line, "verify-nor") == 0) (void)load_and_verify();
    else if (provision_command == MTFS_KEY_PROVISION_COMMAND_INITIAL) provision(0);
    else if (provision_command == MTFS_KEY_PROVISION_COMMAND_REPLACE) provision(1);
    else if (provision_command == MTFS_KEY_PROVISION_COMMAND_LEGACY_UPDATE)
        tm_printf((UB *)"[provision] %s",
            (UB *)mtfs_key_provision_legacy_guidance());
    else if (strcmp(line, "help") == 0) help();
    else if (strncmp(line, "help ", 5U) == 0) help_group(line + 5U);
    else if (line[0] != '\0') tm_printf((UB *)"unknown command; type help\n");
}

static void console_task(INT start_code, void *context)
{
    char line[LINE_BYTES];
    size_t length = 0U;
    int previous_cr = 0;
    (void)start_code;
    (void)context;
    if (mtfs_app_log_enabled(provision_log_level, MTFS_APP_LOG_INFO))
        tm_printf((UB *)"\nmicroT-FS STM32N6570-DK dedicated fleet-key provisioner\n");
    if (mtfs_stm32_saes_init(&crypto_context) != MTFS_STM32_SAES_OK) {
        tm_printf((UB *)"[provision] BLOCKED SAES/RNG init failed\n");
    }
    if (mtfs_app_log_enabled(provision_log_level, MTFS_APP_LOG_INFO)) {
        info();
        help();
    }
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

EXPORT INT usermain(void);

EXPORT INT usermain(void)
{
    T_CTSK task = {
        .tskatr = TA_HLNG | TA_RNG3,
        .task = console_task,
        .itskpri = 9,
        .stksz = 24U * 1024U
    };
    ID task_id = tk_cre_tsk(&task);
    if ((task_id <= 0) || (tk_sta_tsk(task_id, 0) < E_OK))
        tm_printf((UB *)"[provision] console task start FAIL\n");
    for (;;) (void)tk_slp_tsk(TMO_FEVR);
}
