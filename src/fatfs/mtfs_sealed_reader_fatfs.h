#ifndef MTFS_SEALED_READER_FATFS_H
#define MTFS_SEALED_READER_FATFS_H

#include "../mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL

#include <stdint.h>

#include "ff.h"
#include "../extensions/security/sealed_blob/mtfs_sealed_reader.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_SEALED_READER_FATFS_API_VERSION (1U)

/*
 * Return MTFS_OK while the volume is usable, MTFS_ERROR_NO_MEDIA when it is
 * stably absent, or MTFS_ERROR_NOT_READY while insertion/remount is pending.
 * Other results are conservatively reported as MTFS_ERROR_IO.
 */
typedef mtfs_error_t (*mtfs_sealed_reader_fatfs_media_status_fn)(
    void *context);

typedef struct mtfs_sealed_reader_fatfs
{
    uint32_t api_version;
    uint32_t struct_size;
    FIL file;
    FSIZE_t file_size;
    mtfs_sealed_reader_fatfs_media_status_fn media_status;
    void *media_context;
    uint8_t open;
} mtfs_sealed_reader_fatfs_t;

void mtfs_sealed_reader_fatfs_init(mtfs_sealed_reader_fatfs_t *context);

mtfs_error_t mtfs_sealed_reader_fatfs_open(
    mtfs_sealed_reader_fatfs_t *context, const TCHAR *path,
    mtfs_sealed_reader_fatfs_media_status_fn media_status,
    void *media_context, mtfs_sealed_reader_t *reader);

mtfs_error_t mtfs_sealed_reader_fatfs_close(
    mtfs_sealed_reader_fatfs_t *context);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_ENABLE_SEALED_MODEL */

#endif /* MTFS_SEALED_READER_FATFS_H */
