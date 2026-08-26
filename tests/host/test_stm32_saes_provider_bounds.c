#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mtfs_sealed_format.h"
#include "mtfs_stm32_saes_provider.h"

#define GUARD_BYTES (16U)
#define TEST_MODEL_HANDLE (UINT32_C(0x4d4f0001))

static unsigned int lock_calls;
static unsigned int unlock_calls;
static unsigned int decrypt_calls;

static int dummy_read(void *context, uint32_t offset, uint8_t *data,
    size_t bytes)
{
    (void)context;
    (void)offset;
    (void)data;
    (void)bytes;
    return -1;
}

static int dummy_erase(void *context, uint32_t offset)
{
    (void)context;
    (void)offset;
    return -1;
}

static int dummy_program(void *context, uint32_t offset,
    const uint8_t *data, size_t bytes)
{
    (void)context;
    (void)offset;
    (void)data;
    (void)bytes;
    return -1;
}

static int test_lock(void *context)
{
    (void)context;
    ++lock_calls;
    return 0;
}

static void test_unlock(void *context)
{
    (void)context;
    ++unlock_calls;
}

void mtfs_stm32_saes_zeroize(void *memory, size_t bytes)
{
    volatile uint8_t *cursor = (volatile uint8_t *)memory;
    while (bytes != 0U) {
        *cursor++ = 0U;
        --bytes;
    }
}

mtfs_stm32_saes_status_t mtfs_stm32_saes_init(
    mtfs_stm32_saes_context_t *context)
{
    memset(context, 0, sizeof(*context));
    context->rng_ready = 1U;
    return MTFS_STM32_SAES_OK;
}

mtfs_stm32_saes_status_t mtfs_stm32_saes_wrap_key(
    mtfs_stm32_saes_context_t *context,
    const uint8_t raw_key[MTFS_STM32_SAES_AES256_KEY_BYTES],
    mtfs_stm32_saes_wrapped_key_t *wrapped_key)
{
    (void)context;
    (void)raw_key;
    memset(wrapped_key, 0x3c, sizeof(*wrapped_key));
    return MTFS_STM32_SAES_OK;
}

mtfs_stm32_saes_status_t mtfs_stm32_saes_decrypt_wrapped(
    mtfs_stm32_saes_context_t *context,
    const mtfs_stm32_saes_wrapped_key_t *wrapped_key,
    const uint8_t nonce[MTFS_STM32_SAES_GCM_NONCE_BYTES],
    const uint8_t *aad, size_t aad_bytes, const uint8_t *ciphertext,
    size_t ciphertext_bytes,
    const uint8_t tag[MTFS_STM32_SAES_GCM_TAG_BYTES], uint8_t *plaintext)
{
    (void)context;
    (void)wrapped_key;
    (void)nonce;
    (void)aad;
    (void)aad_bytes;
    (void)ciphertext;
    (void)tag;
    ++decrypt_calls;
    memset(plaintext, 0x3c, ciphertext_bytes);
    return MTFS_STM32_SAES_OK;
}

mtfs_stm32_nor_key_store_status_t mtfs_stm32_nor_key_store_load(
    const mtfs_stm32_nor_io_t *io,
    mtfs_stm32_nor_wrapped_key_t *wrapped_key,
    mtfs_stm32_nor_key_metadata_t *metadata,
    mtfs_stm32_nor_key_store_diagnostics_t *diagnostics)
{
    (void)io;
    (void)wrapped_key;
    (void)metadata;
    (void)diagnostics;
    return MTFS_STM32_NOR_KEY_STORE_NOT_FOUND;
}

static int all_value(const uint8_t *data, size_t bytes, uint8_t value)
{
    size_t index;
    for (index = 0U; index < bytes; ++index) {
        if (data[index] != value)
            return 0;
    }
    return 1;
}

static int expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        return 0;
    }
    return 1;
}

int main(void)
{
    static uint8_t ciphertext[MTFS_STM32_SAES_MAX_DATA_BYTES];
    static uint8_t plaintext[MTFS_STM32_SAES_MAX_DATA_BYTES + GUARD_BYTES];
    static uint8_t aad[MTFS_STM32_SAES_MAX_AAD_BYTES + 1U];
    static const uint8_t nonce[MTFS_STM32_SAES_GCM_NONCE_BYTES] = {0U};
    static const uint8_t tag[MTFS_STM32_SAES_GCM_TAG_BYTES] = {0U};
    mtfs_stm32_saes_provider_context_t context;
    mtfs_crypto_provider_t provider;
    mtfs_stm32_nor_io_t nor = {
        NULL, dummy_read, dummy_erase, dummy_program
    };
    mtfs_crypto_status_t status;
    int ok = 1;

    ok &= expect(MTFS_STM32_SAES_MAX_AAD_BYTES >=
        MTFS_SEALED_CHUNK_AAD_SIZE, "primitive AAD capacity covers v1");
    ok &= expect(mtfs_stm32_saes_provider_init(&context, &nor, test_lock,
        test_unlock, NULL, &provider) == MTFS_CRYPTO_OK,
        "provider initialization");
    lock_calls = 0U;
    unlock_calls = 0U;
    decrypt_calls = 0U;
    context.model_open = 1U;
    context.model_handle = TEST_MODEL_HANDLE;

    memset(plaintext, 0xa5, sizeof(plaintext));
    status = provider.decrypt_chunk(provider.context, TEST_MODEL_HANDLE,
        nonce, aad, MTFS_SEALED_CHUNK_AAD_SIZE, ciphertext,
        MTFS_STM32_SAES_MAX_DATA_BYTES, tag, plaintext);
    ok &= expect(status == MTFS_CRYPTO_OK, "64 KiB ciphertext accepted");
    ok &= expect(all_value(plaintext, MTFS_STM32_SAES_MAX_DATA_BYTES, 0x3cU),
        "64 KiB plaintext produced");
    ok &= expect(all_value(plaintext + MTFS_STM32_SAES_MAX_DATA_BYTES,
        GUARD_BYTES, 0xa5U), "64 KiB guard preserved");
    ok &= expect(decrypt_calls == 1U && lock_calls == 1U && unlock_calls == 1U,
        "valid maximum starts one locked SAES operation");

    lock_calls = unlock_calls = decrypt_calls = 0U;
    memset(plaintext, 0xa5, sizeof(plaintext));
    status = provider.decrypt_chunk(provider.context, TEST_MODEL_HANDLE,
        nonce, aad, MTFS_SEALED_CHUNK_AAD_SIZE, ciphertext,
        MTFS_STM32_SAES_MAX_DATA_BYTES + 1U, tag, plaintext);
    ok &= expect(status == MTFS_CRYPTO_RESOURCE_EXHAUSTED,
        "65,537-byte ciphertext rejected");
    ok &= expect(all_value(plaintext, sizeof(plaintext), 0xa5U),
        "65,537-byte rejection leaves output and guard unchanged");
    ok &= expect(decrypt_calls == 0U && lock_calls == 0U && unlock_calls == 0U,
        "65,537-byte rejection does not lock or start SAES");

    memset(plaintext, 0xa5, sizeof(plaintext));
    status = provider.decrypt_chunk(provider.context, TEST_MODEL_HANDLE,
        nonce, aad, MTFS_SEALED_CHUNK_AAD_SIZE, ciphertext, SIZE_MAX, tag,
        plaintext);
    ok &= expect(status == MTFS_CRYPTO_RESOURCE_EXHAUSTED,
        "SIZE_MAX ciphertext rejected");
    ok &= expect(all_value(plaintext, sizeof(plaintext), 0xa5U),
        "SIZE_MAX rejection leaves output unchanged");
    ok &= expect(decrypt_calls == 0U && lock_calls == 0U && unlock_calls == 0U,
        "SIZE_MAX rejection does not lock or start SAES");

    memset(plaintext, 0xa5, sizeof(plaintext));
    status = provider.decrypt_chunk(provider.context, TEST_MODEL_HANDLE,
        nonce, aad, MTFS_SEALED_CHUNK_AAD_SIZE + 1U, ciphertext, 32U, tag,
        plaintext);
    ok &= expect(status == MTFS_CRYPTO_RESOURCE_EXHAUSTED,
        "4,279-byte AAD rejected by sealed provider");
    ok &= expect(all_value(plaintext, 32U, 0U) &&
        all_value(plaintext + 32U, GUARD_BYTES, 0xa5U),
        "AAD rejection zeroizes only the safe output range");
    ok &= expect(decrypt_calls == 0U && lock_calls == 0U && unlock_calls == 0U,
        "AAD rejection does not lock or start SAES");

    memset(plaintext, 0xa5, sizeof(plaintext));
    status = provider.decrypt_chunk(provider.context, TEST_MODEL_HANDLE,
        nonce, aad, MTFS_STM32_SAES_MAX_AAD_BYTES + 1U, ciphertext, 32U,
        tag, plaintext);
    ok &= expect(status == MTFS_CRYPTO_RESOURCE_EXHAUSTED,
        "primitive-capacity AAD overflow rejected safely");
    ok &= expect(all_value(plaintext, 32U, 0U) &&
        all_value(plaintext + 32U, GUARD_BYTES, 0xa5U),
        "primitive-capacity rejection preserves guard");
    ok &= expect(decrypt_calls == 0U && lock_calls == 0U && unlock_calls == 0U,
        "primitive-capacity rejection does not start SAES");

    ok &= expect(mtfs_stm32_saes_provider_deinit(&context) == MTFS_CRYPTO_OK,
        "provider cleanup");
    if (ok)
        puts("STM32 SAES provider boundary tests: PASS");
    return ok ? 0 : 1;
}
