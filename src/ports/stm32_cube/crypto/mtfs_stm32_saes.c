#include "../../../mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL

#include "mtfs_stm32_saes.h"

#include <string.h>

#include "stm32n6xx_hal_cryp_ex.h"
#include "stm32n6xx_hal_rcc.h"

#define MTFS_STM32_SAES_TIMEOUT_MS (1000U)
/* HAL1 takes uint16_t Size.  Non-final GCM calls must end on a block. */
#define MTFS_STM32_SAES_HAL_SEGMENT_BYTES \
    ((uint16_t)(UINT16_MAX & ~(UINT16_C(16) - UINT16_C(1))))

typedef union mtfs_stm32_saes_data_buffer
{
    uint32_t words[(MTFS_STM32_SAES_MAX_DATA_BYTES + 3U) / 4U];
    uint8_t bytes[MTFS_STM32_SAES_MAX_DATA_BYTES];
} mtfs_stm32_saes_data_buffer_t;

typedef union mtfs_stm32_saes_aad_buffer
{
    uint32_t words[(MTFS_STM32_SAES_MAX_AAD_BYTES + 3U) / 4U];
    uint8_t bytes[MTFS_STM32_SAES_MAX_AAD_BYTES];
} mtfs_stm32_saes_aad_buffer_t;

/* The Phase 4.1B spike is deliberately single-threaded and non-reentrant. */
static mtfs_stm32_saes_data_buffer_t input_work;
static mtfs_stm32_saes_data_buffer_t output_work;
static mtfs_stm32_saes_aad_buffer_t aad_work;
static uint32_t iv_work[4];
static uint32_t tag_work[4];
static mtfs_stm32_saes_wrapped_key_t wrapped_work;

void mtfs_stm32_saes_zeroize(void *memory, size_t bytes)
{
    volatile uint8_t *cursor = (volatile uint8_t *)memory;
    while (bytes != 0U) {
        *cursor++ = 0U;
        --bytes;
    }
}

static void saes_reset(void)
{
    __HAL_RCC_SAES_FORCE_RESET();
    __DSB();
    __HAL_RCC_SAES_RELEASE_RESET();
    __DSB();
}

static void clear_work(void)
{
    mtfs_stm32_saes_zeroize(&input_work, sizeof(input_work));
    mtfs_stm32_saes_zeroize(&output_work, sizeof(output_work));
    mtfs_stm32_saes_zeroize(&aad_work, sizeof(aad_work));
    mtfs_stm32_saes_zeroize(iv_work, sizeof(iv_work));
    mtfs_stm32_saes_zeroize(tag_work, sizeof(tag_work));
    mtfs_stm32_saes_zeroize(&wrapped_work, sizeof(wrapped_work));
}

static mtfs_stm32_saes_status_t record_hal(
    mtfs_stm32_saes_context_t *context,
    HAL_StatusTypeDef status,
    mtfs_stm32_saes_hal_operation_t operation)
{
    context->last_hal_status = (uint32_t)status;
    context->last_hal_error = context->cryp.ErrorCode;
    context->last_saes_cr = SAES->CR;
    context->last_saes_sr = SAES->SR;
    context->last_saes_isr = SAES->ISR;
    context->last_hal_operation = operation;
    return (status == HAL_OK) ? MTFS_STM32_SAES_OK :
        MTFS_STM32_SAES_HAL_ERROR;
}

static void finish_operation(mtfs_stm32_saes_context_t *context)
{
    (void)HAL_CRYP_DeInit(&context->cryp);
    saes_reset();
    mtfs_stm32_saes_zeroize(&context->cryp, sizeof(context->cryp));
    clear_work();
}

static void pack_iv(const uint8_t nonce[MTFS_STM32_SAES_GCM_NONCE_BYTES])
{
    size_t word;
    for (word = 0U; word < 3U; ++word) {
        size_t offset = word * 4U;
        iv_work[word] = ((uint32_t)nonce[offset] << 24) |
            ((uint32_t)nonce[offset + 1U] << 16) |
            ((uint32_t)nonce[offset + 2U] << 8) |
            (uint32_t)nonce[offset + 3U];
    }
    /* GCM J0 + 1 for a 96-bit IV, as required by the STM32 HAL. */
    iv_work[3] = UINT32_C(2);
}

static void pack_key(const uint8_t raw_key[MTFS_STM32_SAES_AES256_KEY_BYTES])
{
    size_t word;
    for (word = 0U; word < 8U; ++word) {
        size_t offset = word * 4U;
        input_work.words[word] = ((uint32_t)raw_key[offset] << 24) |
            ((uint32_t)raw_key[offset + 1U] << 16) |
            ((uint32_t)raw_key[offset + 2U] << 8) |
            (uint32_t)raw_key[offset + 3U];
    }
}

static void select_byte_data_type(mtfs_stm32_saes_context_t *context)
{
    /* Wrap/unwrap blobs are native words. GCM input/AAD are byte strings. */
    context->cryp.Init.DataType = CRYP_DATATYPE_8B;
    MODIFY_REG(SAES->CR, SAES_CR_DATATYPE,
        SAES_CONV_DATATYPE(CRYP_DATATYPE_8B));
}

static void copy_tag(uint8_t tag[MTFS_STM32_SAES_GCM_TAG_BYTES])
{
    /* CRYP_DATATYPE_8B already converts DOUTR to byte-string memory order. */
    memcpy(tag, tag_work, MTFS_STM32_SAES_GCM_TAG_BYTES);
}

static int constant_time_equal(const uint8_t *left, const uint8_t *right,
    size_t bytes)
{
    uint8_t difference = 0U;
    size_t index;
    for (index = 0U; index < bytes; ++index) {
        difference |= (uint8_t)(left[index] ^ right[index]);
    }
    return difference == 0U;
}

mtfs_stm32_saes_status_t mtfs_stm32_saes_init(
    mtfs_stm32_saes_context_t *context)
{
    HAL_StatusTypeDef status;
    if (context == NULL) {
        return MTFS_STM32_SAES_INVALID_ARGUMENT;
    }
    mtfs_stm32_saes_zeroize(context, sizeof(*context));
    clear_work();
    __HAL_RCC_SAES_CLK_ENABLE();
    __HAL_RCC_RNG_CLK_ENABLE();
    saes_reset();
    context->rng.Instance = RNG;
    context->rng.Init.ClockErrorDetection = RNG_CED_ENABLE;
    status = HAL_RNG_Init(&context->rng);
    context->last_hal_status = (uint32_t)status;
    if (status != HAL_OK) {
        return MTFS_STM32_SAES_HAL_ERROR;
    }
    context->rng_ready = 1U;
    return MTFS_STM32_SAES_OK;
}

static mtfs_stm32_saes_status_t prepare(
    mtfs_stm32_saes_context_t *context,
    const mtfs_stm32_saes_wrapped_key_t *wrapped_key,
    const uint8_t nonce[MTFS_STM32_SAES_GCM_NONCE_BYTES],
    const uint8_t *aad,
    size_t aad_bytes)
{
    HAL_StatusTypeDef status;
    mtfs_stm32_saes_zeroize(&context->cryp, sizeof(context->cryp));
    context->cryp.Instance = SAES;
    context->cryp.Init.DataType = CRYP_DATATYPE_32B;
    context->cryp.Init.KeySize = CRYP_KEYSIZE_256B;
    context->cryp.Init.pKey = NULL;
    context->cryp.Init.pInitVect = iv_work;
    /*
     * Unwrap is an ECB operation even when the resulting key will be used by
     * GCM.  The N6 HAL otherwise enters key-derivation mode with CHMOD=GCM;
     * SAES never raises CCF for that unsupported combination and the HAL
     * reports HAL_CRYP_ERROR_TIMEOUT before consuming the wrapped key.
     */
    context->cryp.Init.Algorithm = CRYP_AES_ECB;
    context->cryp.Init.Header = aad_work.words;
    context->cryp.Init.HeaderSize = (uint32_t)aad_bytes;
    context->cryp.Init.DataWidthUnit = CRYP_DATAWIDTHUNIT_BYTE;
    context->cryp.Init.HeaderWidthUnit = CRYP_HEADERWIDTHUNIT_BYTE;
    context->cryp.Init.KeyIVConfigSkip = CRYP_KEYIVCONFIG_ALWAYS;
    context->cryp.Init.KeyMode = CRYP_KEYMODE_WRAPPED;
    context->cryp.Init.KeySelect = CRYP_KEYSEL_HW;
    context->cryp.Init.KeyProtection = CRYP_KEYPROT_DISABLE;
    pack_iv(nonce);
    if (aad_bytes != 0U) {
        memcpy(aad_work.bytes, aad, aad_bytes);
    }
    memcpy(&wrapped_work, wrapped_key, sizeof(wrapped_work));
    status = HAL_CRYP_Init(&context->cryp);
    if (record_hal(context, status, MTFS_STM32_SAES_HAL_INIT) !=
        MTFS_STM32_SAES_OK) {
        return MTFS_STM32_SAES_HAL_ERROR;
    }
    status = HAL_CRYPEx_UnwrapKey(&context->cryp, wrapped_work.words,
        MTFS_STM32_SAES_TIMEOUT_MS);
    if (record_hal(context, status, MTFS_STM32_SAES_HAL_UNWRAP) !=
        MTFS_STM32_SAES_OK) {
        return MTFS_STM32_SAES_HAL_ERROR;
    }
    /*
     * Keep the unwrapped key inside SAES, but leave wrapped-key mode before
     * using that key for ordinary data.  HAL2 does this at every data
     * encrypt/decrypt entry; the N6 HAL1 driver does not.
     */
    context->cryp.Init.Algorithm = CRYP_AES_GCM;
    context->cryp.Init.KeyMode = CRYP_KEYMODE_NORMAL;
    context->cryp.Init.KeyIVConfigSkip = CRYP_KEYIVCONFIG_ONCE;
    context->cryp.KeyIVConfig = 0U;
    context->cryp.SizesSum = 0U;
    MODIFY_REG(SAES->CR, SAES_CR_CHMOD | SAES_CR_KMOD,
        SAES_CR_CHMOD_AES_GCM | CRYP_KEYMODE_NORMAL);
    select_byte_data_type(context);
    return MTFS_STM32_SAES_OK;
}

static mtfs_stm32_saes_status_t process_payload(
    mtfs_stm32_saes_context_t *context, size_t bytes, int encrypt)
{
    size_t offset = 0U;

    do {
        size_t remaining = bytes - offset;
        uint16_t segment = (remaining > UINT16_MAX) ?
            MTFS_STM32_SAES_HAL_SEGMENT_BYTES : (uint16_t)remaining;
        HAL_StatusTypeDef hal_status;

        if (encrypt) {
            hal_status = HAL_CRYP_Encrypt(&context->cryp,
                input_work.words + (offset / sizeof(uint32_t)), segment,
                output_work.words + (offset / sizeof(uint32_t)),
                MTFS_STM32_SAES_TIMEOUT_MS);
        } else {
            hal_status = HAL_CRYP_Decrypt(&context->cryp,
                input_work.words + (offset / sizeof(uint32_t)), segment,
                output_work.words + (offset / sizeof(uint32_t)),
                MTFS_STM32_SAES_TIMEOUT_MS);
        }
        if (record_hal(context, hal_status, encrypt ?
            MTFS_STM32_SAES_HAL_ENCRYPT : MTFS_STM32_SAES_HAL_DECRYPT) !=
            MTFS_STM32_SAES_OK) {
            return MTFS_STM32_SAES_HAL_ERROR;
        }
        offset += segment;
    } while (offset < bytes);

    return MTFS_STM32_SAES_OK;
}

mtfs_stm32_saes_status_t mtfs_stm32_saes_wrap_key(
    mtfs_stm32_saes_context_t *context,
    const uint8_t raw_key[MTFS_STM32_SAES_AES256_KEY_BYTES],
    mtfs_stm32_saes_wrapped_key_t *wrapped_key)
{
    HAL_StatusTypeDef hal_status;
    mtfs_stm32_saes_status_t status;
    if ((context == NULL) || (raw_key == NULL) || (wrapped_key == NULL) ||
        (context->rng_ready == 0U)) {
        return MTFS_STM32_SAES_INVALID_ARGUMENT;
    }
    pack_key(raw_key);
    mtfs_stm32_saes_zeroize(&context->cryp, sizeof(context->cryp));
    context->cryp.Instance = SAES;
    context->cryp.Init.DataType = CRYP_DATATYPE_32B;
    context->cryp.Init.KeySize = CRYP_KEYSIZE_256B;
    context->cryp.Init.Algorithm = CRYP_AES_ECB;
    context->cryp.Init.DataWidthUnit = CRYP_DATAWIDTHUNIT_WORD;
    context->cryp.Init.HeaderWidthUnit = CRYP_HEADERWIDTHUNIT_WORD;
    context->cryp.Init.KeyIVConfigSkip = CRYP_KEYIVCONFIG_ALWAYS;
    context->cryp.Init.KeyMode = CRYP_KEYMODE_WRAPPED;
    context->cryp.Init.KeySelect = CRYP_KEYSEL_HW;
    context->cryp.Init.KeyProtection = CRYP_KEYPROT_DISABLE;
    hal_status = HAL_CRYP_Init(&context->cryp);
    status = record_hal(context, hal_status, MTFS_STM32_SAES_HAL_INIT);
    if (status == MTFS_STM32_SAES_OK) {
        hal_status = HAL_CRYPEx_WrapKey(&context->cryp, input_work.words,
            wrapped_work.words, MTFS_STM32_SAES_TIMEOUT_MS);
        status = record_hal(context, hal_status, MTFS_STM32_SAES_HAL_WRAP);
    }
    if (status == MTFS_STM32_SAES_OK) {
        memcpy(wrapped_key, &wrapped_work, sizeof(*wrapped_key));
    } else {
        mtfs_stm32_saes_zeroize(wrapped_key, sizeof(*wrapped_key));
    }
    finish_operation(context);
    return status;
}

static mtfs_stm32_saes_status_t validate_gcm_arguments(
    mtfs_stm32_saes_context_t *context,
    const mtfs_stm32_saes_wrapped_key_t *wrapped_key,
    const uint8_t *nonce,
    const uint8_t *aad,
    size_t aad_bytes,
    const uint8_t *input,
    size_t input_bytes,
    uint8_t *output)
{
    if ((context == NULL) || (wrapped_key == NULL) || (nonce == NULL) ||
        (output == NULL) || (context->rng_ready == 0U) ||
        ((aad == NULL) && (aad_bytes != 0U)) ||
        ((input == NULL) && (input_bytes != 0U))) {
        return MTFS_STM32_SAES_INVALID_ARGUMENT;
    }
    if ((input_bytes > MTFS_STM32_SAES_MAX_DATA_BYTES) ||
        (aad_bytes > MTFS_STM32_SAES_MAX_AAD_BYTES)) {
        return MTFS_STM32_SAES_TOO_LARGE;
    }
    return MTFS_STM32_SAES_OK;
}

mtfs_stm32_saes_status_t mtfs_stm32_saes_encrypt_wrapped(
    mtfs_stm32_saes_context_t *context,
    const mtfs_stm32_saes_wrapped_key_t *wrapped_key,
    const uint8_t nonce[MTFS_STM32_SAES_GCM_NONCE_BYTES],
    const uint8_t *aad, size_t aad_bytes,
    const uint8_t *plaintext, size_t plaintext_bytes,
    uint8_t *ciphertext, uint8_t tag[MTFS_STM32_SAES_GCM_TAG_BYTES])
{
    mtfs_stm32_saes_status_t status = validate_gcm_arguments(context,
        wrapped_key, nonce, aad, aad_bytes, plaintext, plaintext_bytes,
        ciphertext);
    HAL_StatusTypeDef hal_status;
    if (tag == NULL) {
        return MTFS_STM32_SAES_INVALID_ARGUMENT;
    }
    if (status != MTFS_STM32_SAES_OK) {
        return status;
    }
    if (plaintext_bytes != 0U) {
        memcpy(input_work.bytes, plaintext, plaintext_bytes);
    }
    status = prepare(context, wrapped_key, nonce, aad, aad_bytes);
    if (status == MTFS_STM32_SAES_OK) {
        status = process_payload(context, plaintext_bytes, 1);
    }
    if (status == MTFS_STM32_SAES_OK) {
        hal_status = HAL_CRYPEx_AESGCM_GenerateAuthTAG(&context->cryp,
            tag_work, MTFS_STM32_SAES_TIMEOUT_MS);
        status = record_hal(context, hal_status, MTFS_STM32_SAES_HAL_TAG);
    }
    if (status == MTFS_STM32_SAES_OK) {
        if (plaintext_bytes != 0U) {
            memcpy(ciphertext, output_work.bytes, plaintext_bytes);
        }
        copy_tag(tag);
    } else {
        mtfs_stm32_saes_zeroize(ciphertext, plaintext_bytes);
        mtfs_stm32_saes_zeroize(tag, MTFS_STM32_SAES_GCM_TAG_BYTES);
    }
    finish_operation(context);
    return status;
}

mtfs_stm32_saes_status_t mtfs_stm32_saes_decrypt_wrapped(
    mtfs_stm32_saes_context_t *context,
    const mtfs_stm32_saes_wrapped_key_t *wrapped_key,
    const uint8_t nonce[MTFS_STM32_SAES_GCM_NONCE_BYTES],
    const uint8_t *aad, size_t aad_bytes,
    const uint8_t *ciphertext, size_t ciphertext_bytes,
    const uint8_t tag[MTFS_STM32_SAES_GCM_TAG_BYTES], uint8_t *plaintext)
{
    uint8_t generated_tag[MTFS_STM32_SAES_GCM_TAG_BYTES];
    mtfs_stm32_saes_status_t status = validate_gcm_arguments(context,
        wrapped_key, nonce, aad, aad_bytes, ciphertext, ciphertext_bytes,
        plaintext);
    HAL_StatusTypeDef hal_status;
    if (tag == NULL) {
        return MTFS_STM32_SAES_INVALID_ARGUMENT;
    }
    if (status != MTFS_STM32_SAES_OK) {
        return status;
    }
    if (ciphertext_bytes != 0U) {
        memcpy(input_work.bytes, ciphertext, ciphertext_bytes);
    }
    status = prepare(context, wrapped_key, nonce, aad, aad_bytes);
    if (status == MTFS_STM32_SAES_OK) {
        status = process_payload(context, ciphertext_bytes, 0);
    }
    if (status == MTFS_STM32_SAES_OK) {
        hal_status = HAL_CRYPEx_AESGCM_GenerateAuthTAG(&context->cryp,
            tag_work, MTFS_STM32_SAES_TIMEOUT_MS);
        status = record_hal(context, hal_status, MTFS_STM32_SAES_HAL_TAG);
    }
    if (status == MTFS_STM32_SAES_OK) {
        copy_tag(generated_tag);
        if (!constant_time_equal(generated_tag, tag, sizeof(generated_tag))) {
            status = MTFS_STM32_SAES_AUTH_FAILED;
        }
    }
    if (status == MTFS_STM32_SAES_OK) {
        if (ciphertext_bytes != 0U) {
            memcpy(plaintext, output_work.bytes, ciphertext_bytes);
        }
    } else {
        mtfs_stm32_saes_zeroize(plaintext, ciphertext_bytes);
    }
    mtfs_stm32_saes_zeroize(generated_tag, sizeof(generated_tag));
    finish_operation(context);
    return status;
}

const char *mtfs_stm32_saes_status_string(mtfs_stm32_saes_status_t status)
{
    switch (status) {
    case MTFS_STM32_SAES_OK: return "ok";
    case MTFS_STM32_SAES_INVALID_ARGUMENT: return "invalid-argument";
    case MTFS_STM32_SAES_TOO_LARGE: return "too-large";
    case MTFS_STM32_SAES_HAL_ERROR: return "hal-error";
    case MTFS_STM32_SAES_AUTH_FAILED: return "auth-failed";
    default: return "unknown";
    }
}

const char *mtfs_stm32_saes_hal_operation_string(
    mtfs_stm32_saes_hal_operation_t operation)
{
    switch (operation) {
    case MTFS_STM32_SAES_HAL_NONE: return "none";
    case MTFS_STM32_SAES_HAL_INIT: return "init";
    case MTFS_STM32_SAES_HAL_WRAP: return "wrap";
    case MTFS_STM32_SAES_HAL_UNWRAP: return "unwrap";
    case MTFS_STM32_SAES_HAL_ENCRYPT: return "encrypt";
    case MTFS_STM32_SAES_HAL_DECRYPT: return "decrypt";
    case MTFS_STM32_SAES_HAL_TAG: return "tag";
    default: return "unknown";
    }
}

#else
typedef int mtfs_stm32_saes_disabled_translation_unit_t;
#endif /* MTFS_ENABLE_SEALED_MODEL */
