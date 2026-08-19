#include "mtfs_ra_rsip_key_file.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void secure_zero(void *data, size_t data_bytes)
{
    volatile uint8_t *cursor = (volatile uint8_t *)data;

    while (data_bytes != 0U) {
        *cursor++ = 0U;
        --data_bytes;
    }
#if defined(__GNUC__)
    __asm volatile ("" : : "r" (data) : "memory");
#endif
}

void mtfs_ra_rsip_key_file_init(mtfs_ra_rsip_key_file_t *context)
{
    if (context != NULL) {
        memset(context, 0, sizeof(*context));
        context->last_load_status = MTFS_WRAPPED_KEY_FATFS_NOT_FOUND;
        context->diagnostics.last_record_status =
            MTFS_WRAPPED_KEY_RECORD_INVALID_FORMAT;
    }
}

mtfs_error_t mtfs_ra_rsip_key_file_load(
    mtfs_ra_rsip_key_file_t *context, const char *path)
{
    const uint8_t *blob = NULL;
    mtfs_wrapped_key_fatfs_status_t status;

    if ((context == NULL) || (path == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    mtfs_ra_rsip_key_file_unload(context);
    status = mtfs_wrapped_key_fatfs_load(path, context->record,
        sizeof(context->record),
        MTFS_WRAPPED_KEY_PROVIDER_RA_RSIP_E50D,
        MTFS_WRAPPED_KEY_TYPE_AES_256,
        MTFS_WRAPPED_KEY_RECORD_RSIP_AES256_BLOB_BYTES,
        &context->metadata, &blob, &context->diagnostics);
    context->last_load_status = status;
    if (status != MTFS_WRAPPED_KEY_FATFS_OK) {
        if (status == MTFS_WRAPPED_KEY_FATFS_NOT_FOUND) {
            return MTFS_ERROR_NOT_FOUND;
        }
        if (status == MTFS_WRAPPED_KEY_FATFS_INVALID_ARGUMENT) {
            return MTFS_ERROR_INVALID_ARGUMENT;
        }
        return MTFS_ERROR_IO;
    }
    if (((uintptr_t)blob & 15U) != 0U) {
        secure_zero(context->record, sizeof(context->record));
        context->last_load_status = MTFS_WRAPPED_KEY_FATFS_INVALID_RECORD;
        return MTFS_ERROR_IO;
    }
    context->wrapped_key.type = RSIP_KEY_TYPE_AES_256;
    context->wrapped_key.p_value = (void *)blob;
    context->loaded = 1U;
    return MTFS_OK;
}

void mtfs_ra_rsip_key_file_unload(mtfs_ra_rsip_key_file_t *context)
{
    if (context == NULL) {
        return;
    }
    secure_zero(context->record, sizeof(context->record));
    secure_zero(&context->metadata, sizeof(context->metadata));
    context->wrapped_key.type = RSIP_KEY_TYPE_AES_256;
    context->wrapped_key.p_value = NULL;
    context->loaded = 0U;
}

const rsip_wrapped_key_t *mtfs_ra_rsip_key_file_key(
    const mtfs_ra_rsip_key_file_t *context)
{
    if ((context == NULL) || !context->loaded ||
        (context->wrapped_key.p_value == NULL)) {
        return NULL;
    }
    return &context->wrapped_key;
}
