#include "../mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL

#include "mtfs_sealed_reader_fatfs.h"

#include <limits.h>
#include <string.h>

static mtfs_error_t media_result(mtfs_sealed_reader_fatfs_t *context)
{
    mtfs_error_t result;
    if (context->media_status == NULL)
        return MTFS_OK;
    result = context->media_status(context->media_context);
    if (result == MTFS_OK || result == MTFS_ERROR_NO_MEDIA ||
        result == MTFS_ERROR_NOT_READY)
        return result;
    return MTFS_ERROR_IO;
}

static mtfs_error_t fatfs_result(mtfs_sealed_reader_fatfs_t *context,
                                 FRESULT result)
{
    mtfs_error_t media;
    if (result == FR_OK)
        return MTFS_OK;
    media = media_result(context);
    if (media != MTFS_OK)
        return media;
    if (result == FR_NO_FILE || result == FR_NO_PATH)
        return MTFS_ERROR_NOT_FOUND;
    if (result == FR_NOT_READY)
        return MTFS_ERROR_NOT_READY;
    if (result == FR_INVALID_PARAMETER)
        return MTFS_ERROR_INVALID_ARGUMENT;
    return MTFS_ERROR_IO;
}

static int valid_context(const mtfs_sealed_reader_fatfs_t *context)
{
    return context != NULL &&
        context->api_version == MTFS_SEALED_READER_FATFS_API_VERSION &&
        context->struct_size >= sizeof(*context);
}

static mtfs_error_t get_size(void *opaque, uint64_t *size)
{
    mtfs_sealed_reader_fatfs_t *context =
        (mtfs_sealed_reader_fatfs_t *)opaque;
    mtfs_error_t result;
    if (!valid_context(context) || size == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (context->open == 0U)
        return MTFS_ERROR_INVALID_STATE;
    result = media_result(context);
    if (result != MTFS_OK)
        return result;
    *size = (uint64_t)context->file_size;
    return MTFS_OK;
}

static mtfs_error_t read_at(void *opaque, uint64_t offset, void *buffer,
                            size_t requested, size_t *read_size)
{
    mtfs_sealed_reader_fatfs_t *context =
        (mtfs_sealed_reader_fatfs_t *)opaque;
    uint64_t maximum_fsize = (uint64_t)(FSIZE_t)~(FSIZE_t)0;
    UINT received = 0U;
    FRESULT fat_result;
    mtfs_error_t result;
    if (read_size != NULL)
        *read_size = 0U;
    if (!valid_context(context) || read_size == NULL ||
        (buffer == NULL && requested != 0U))
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (context->open == 0U)
        return MTFS_ERROR_INVALID_STATE;
    if (requested > UINT_MAX || offset > maximum_fsize ||
        (uint64_t)requested > UINT64_MAX - offset)
        return MTFS_ERROR_OVERFLOW;
    if (offset + (uint64_t)requested > (uint64_t)context->file_size)
        return MTFS_ERROR_IO;
    result = media_result(context);
    if (result != MTFS_OK)
        return result;
    fat_result = f_lseek(&context->file, (FSIZE_t)offset);
    result = fatfs_result(context, fat_result);
    if (result != MTFS_OK)
        return result;
    if ((uint64_t)f_tell(&context->file) != offset)
        return MTFS_ERROR_IO;
    fat_result = f_read(&context->file, buffer, (UINT)requested, &received);
    result = fatfs_result(context, fat_result);
    if (result != MTFS_OK)
        return result;
    *read_size = (size_t)received;
    return received == (UINT)requested ? MTFS_OK : MTFS_ERROR_IO;
}

void mtfs_sealed_reader_fatfs_init(mtfs_sealed_reader_fatfs_t *context)
{
    if (context != NULL) {
        memset(context, 0, sizeof(*context));
        context->api_version = MTFS_SEALED_READER_FATFS_API_VERSION;
        context->struct_size = (uint32_t)sizeof(*context);
    }
}

mtfs_error_t mtfs_sealed_reader_fatfs_open(
    mtfs_sealed_reader_fatfs_t *context, const TCHAR *path,
    mtfs_sealed_reader_fatfs_media_status_fn status, void *status_context,
    mtfs_sealed_reader_t *reader)
{
    FRESULT fat_result;
    mtfs_error_t result;
    if (!valid_context(context) || path == NULL || reader == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (context->open != 0U)
        return MTFS_ERROR_INVALID_STATE;
    context->media_status = status;
    context->media_context = status_context;
    result = media_result(context);
    if (result != MTFS_OK)
        goto failure;
    fat_result = f_open(&context->file, path, FA_READ | FA_OPEN_EXISTING);
    result = fatfs_result(context, fat_result);
    if (result != MTFS_OK)
        goto failure;
    context->file_size = f_size(&context->file);
    context->open = 1U;
    reader->api_version = MTFS_SEALED_READER_API_VERSION;
    reader->struct_size = (uint32_t)sizeof(*reader);
    reader->context = context;
    reader->get_size = get_size;
    reader->read_at = read_at;
    return MTFS_OK;

failure:
    memset(&context->file, 0, sizeof(context->file));
    context->file_size = 0;
    context->media_status = NULL;
    context->media_context = NULL;
    memset(reader, 0, sizeof(*reader));
    return result;
}

mtfs_error_t mtfs_sealed_reader_fatfs_close(
    mtfs_sealed_reader_fatfs_t *context)
{
    FRESULT fat_result = FR_OK;
    mtfs_error_t result = MTFS_OK;
    if (!valid_context(context))
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (context->open != 0U) {
        fat_result = f_close(&context->file);
        result = fatfs_result(context, fat_result);
    }
    mtfs_sealed_reader_fatfs_init(context);
    return result;
}

#else
typedef int mtfs_sealed_reader_fatfs_disabled_translation_unit_t;
#endif /* MTFS_ENABLE_SEALED_MODEL */
