#include "mtfs_host_block_file.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static mtfs_error_t mtfs_host_initialize(void *opaque);
static mtfs_error_t mtfs_host_status(void *opaque, mtfs_block_status_t *status);
static mtfs_error_t mtfs_host_read(
    void *opaque,
    void *buffer,
    mtfs_lba_t lba,
    uint32_t count);
static mtfs_error_t mtfs_host_write(
    void *opaque,
    const void *buffer,
    mtfs_lba_t lba,
    uint32_t count);
static mtfs_error_t mtfs_host_sync(void *opaque);
static mtfs_error_t mtfs_host_get_geometry(
    void *opaque,
    mtfs_block_geometry_t *geometry);

static const mtfs_block_device_ops_t mtfs_host_block_ops = {
    mtfs_host_initialize,
    mtfs_host_status,
    mtfs_host_read,
    mtfs_host_write,
    mtfs_host_sync,
    mtfs_host_get_geometry,
    NULL
};

static mtfs_error_t mtfs_host_check_access(
    const mtfs_host_block_file_t *context,
    mtfs_lba_t lba,
    uint32_t count,
    long *offset)
{
    mtfs_lba_t byte_offset;

    if ((context == NULL) || !context->is_open || (context->stream == NULL)) {
        return MTFS_ERROR_NO_MEDIA;
    }
    if (!context->initialized) {
        return MTFS_ERROR_NOT_READY;
    }
    if ((count == 0U) || (lba >= context->geometry.sector_count) ||
        ((mtfs_lba_t)count > (context->geometry.sector_count - lba))) {
        return (count == 0U) ? MTFS_ERROR_INVALID_ARGUMENT : MTFS_ERROR_OUT_OF_RANGE;
    }
    if (lba > (UINT64_MAX / context->geometry.sector_size)) {
        return MTFS_ERROR_OUT_OF_RANGE;
    }
    byte_offset = lba * context->geometry.sector_size;
    if (byte_offset > (mtfs_lba_t)LONG_MAX) {
        return MTFS_ERROR_OUT_OF_RANGE;
    }
    *offset = (long)byte_offset;
    return MTFS_OK;
}

mtfs_error_t mtfs_host_block_file_open(
    mtfs_host_block_file_t *context,
    mtfs_block_device_t *device,
    const char *path,
    uint32_t sector_size,
    int read_only)
{
    FILE *stream;
    long file_size;

    if ((context == NULL) || (device == NULL) || (path == NULL) ||
        (path[0] == '\0') || (sector_size == 0U)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }

    memset(context, 0, sizeof(*context));
    memset(device, 0, sizeof(*device));
    stream = fopen(path, read_only ? "rb" : "r+b");
    if (stream == NULL) {
        return MTFS_ERROR_NO_MEDIA;
    }
    if ((fseek(stream, 0L, SEEK_END) != 0) || ((file_size = ftell(stream)) < 0L) ||
        (fseek(stream, 0L, SEEK_SET) != 0)) {
        (void)fclose(stream);
        return MTFS_ERROR_IO;
    }
    if ((file_size == 0L) || (((unsigned long)file_size % sector_size) != 0UL)) {
        (void)fclose(stream);
        return MTFS_ERROR_INVALID_ARGUMENT;
    }

    context->stream = stream;
    context->geometry.sector_size = sector_size;
    context->geometry.sector_count = (mtfs_lba_t)((unsigned long)file_size / sector_size);
    context->geometry.erase_block_size = 1U;
    context->is_open = 1;
    context->read_only = read_only ? 1 : 0;

    device->ops = &mtfs_host_block_ops;
    device->context = context;
    device->capabilities = read_only ? MTFS_BLOCK_CAPABILITY_READ_ONLY : 0U;
    return MTFS_OK;
}

mtfs_error_t mtfs_host_block_file_close(mtfs_host_block_file_t *context)
{
    FILE *stream;
    int flush_result = 0;
    int close_result;

    if ((context == NULL) || !context->is_open || (context->stream == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    stream = (FILE *)context->stream;
    if (!context->read_only) {
        flush_result = fflush(stream);
    }
    close_result = fclose(stream);
    memset(context, 0, sizeof(*context));
    return ((flush_result == 0) && (close_result == 0)) ? MTFS_OK : MTFS_ERROR_IO;
}

static mtfs_error_t mtfs_host_initialize(void *opaque)
{
    mtfs_host_block_file_t *context = (mtfs_host_block_file_t *)opaque;

    if ((context == NULL) || !context->is_open || (context->stream == NULL)) {
        return MTFS_ERROR_NO_MEDIA;
    }
    context->initialized = 1;
    return MTFS_OK;
}

static mtfs_error_t mtfs_host_status(void *opaque, mtfs_block_status_t *status)
{
    mtfs_host_block_file_t *context = (mtfs_host_block_file_t *)opaque;

    if ((context == NULL) || (status == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    *status = 0U;
    if (!context->is_open || (context->stream == NULL)) {
        return MTFS_OK;
    }
    *status |= MTFS_BLOCK_STATUS_MEDIA_PRESENT;
    if (context->initialized) {
        *status |= MTFS_BLOCK_STATUS_INITIALIZED;
    }
    if (context->read_only) {
        *status |= MTFS_BLOCK_STATUS_WRITE_PROTECTED;
    }
    return MTFS_OK;
}

static mtfs_error_t mtfs_host_read(
    void *opaque,
    void *buffer,
    mtfs_lba_t lba,
    uint32_t count)
{
    mtfs_host_block_file_t *context = (mtfs_host_block_file_t *)opaque;
    FILE *stream;
    long offset;
    mtfs_error_t result;

    if (buffer == NULL) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    result = mtfs_host_check_access(context, lba, count, &offset);
    if (result != MTFS_OK) {
        return result;
    }
    if ((uint32_t)(size_t)count != count) {
        return MTFS_ERROR_OUT_OF_RANGE;
    }
    if ((size_t)count > (SIZE_MAX / context->geometry.sector_size)) {
        return MTFS_ERROR_OUT_OF_RANGE;
    }
    stream = (FILE *)context->stream;
    if (fseek(stream, offset, SEEK_SET) != 0) {
        return MTFS_ERROR_IO;
    }
    if (fread(buffer, context->geometry.sector_size, (size_t)count, stream) != (size_t)count) {
        return MTFS_ERROR_IO;
    }
    return MTFS_OK;
}

static mtfs_error_t mtfs_host_write(
    void *opaque,
    const void *buffer,
    mtfs_lba_t lba,
    uint32_t count)
{
    mtfs_host_block_file_t *context = (mtfs_host_block_file_t *)opaque;
    FILE *stream;
    long offset;
    mtfs_error_t result;

    if (buffer == NULL) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    if ((context != NULL) && context->read_only) {
        return MTFS_ERROR_WRITE_PROTECTED;
    }
    result = mtfs_host_check_access(context, lba, count, &offset);
    if (result != MTFS_OK) {
        return result;
    }
    if ((uint32_t)(size_t)count != count) {
        return MTFS_ERROR_OUT_OF_RANGE;
    }
    if ((size_t)count > (SIZE_MAX / context->geometry.sector_size)) {
        return MTFS_ERROR_OUT_OF_RANGE;
    }
    stream = (FILE *)context->stream;
    if (fseek(stream, offset, SEEK_SET) != 0) {
        return MTFS_ERROR_IO;
    }
    if (fwrite(buffer, context->geometry.sector_size, (size_t)count, stream) != (size_t)count) {
        return MTFS_ERROR_IO;
    }
    return MTFS_OK;
}

static mtfs_error_t mtfs_host_sync(void *opaque)
{
    mtfs_host_block_file_t *context = (mtfs_host_block_file_t *)opaque;

    if ((context == NULL) || !context->is_open || (context->stream == NULL)) {
        return MTFS_ERROR_NO_MEDIA;
    }
    if (context->read_only) {
        return MTFS_OK;
    }
    return (fflush((FILE *)context->stream) == 0) ? MTFS_OK : MTFS_ERROR_IO;
}

static mtfs_error_t mtfs_host_get_geometry(
    void *opaque,
    mtfs_block_geometry_t *geometry)
{
    mtfs_host_block_file_t *context = (mtfs_host_block_file_t *)opaque;

    if ((context == NULL) || (geometry == NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    if (!context->is_open || (context->stream == NULL)) {
        return MTFS_ERROR_NO_MEDIA;
    }
    *geometry = context->geometry;
    return MTFS_OK;
}
