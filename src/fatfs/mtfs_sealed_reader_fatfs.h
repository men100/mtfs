/** @file mtfs_sealed_reader_fatfs.h
 * @brief FatFs-backed sealed reader adapter. / FatFsを使用するsealed reader adapter。
 * @ingroup mtfs_fatfs */
#ifndef MTFS_SEALED_READER_FATFS_H
#define MTFS_SEALED_READER_FATFS_H

/** @addtogroup mtfs_fatfs
 * @{ */

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
 * Return MTFS_OK while the volume is usable, MTFS_ERROR_NO_MEDIA when it is stably absent, or MTFS_ERROR_NOT_READY while insertion/remount is pending. Other results are conservatively reported as MTFS_ERROR_IO.
 */
/*
 * volumeが使用可能な場合はMTFS_OK、mediaが取り外された状態で安定している場合は MTFS_ERROR_NO_MEDIA、挿入またはremount処理中の場合はMTFS_ERROR_NOT_READYを返す。その他の状態は安全側に倒してMTFS_ERROR_IOとして扱う。
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

/** @brief Initialize a closed caller-owned adapter. / 呼び出し側が所有するadapterをclosed状態で初期化する。
 * @param context Adapter storage. / adapter用領域。 */
void mtfs_sealed_reader_fatfs_init(mtfs_sealed_reader_fatfs_t *context);

/** @brief Open a file and publish a borrowed sealed-reader interface. / fileをopenし、adapterを参照する非所有のsealed reader interfaceを返す。
 * @param context Initialized adapter kept alive until close. / closeまで有効な状態を維持する初期化済みadapter。
 * @param path FatFs path. / FatFs path。
 * @param media_status Optional cached media-status callback; may be NULL. / cache済みのmedia statusを取得する省略可能なcallback。NULL指定可。
 * @param media_context Callback context. / callback context。
 * @param[out] reader Reader whose callbacks refer to context. / callbackからcontextを参照するreaderの出力先。
 * @return MTFS_OK or media/FatFs/state error. / MTFS_OKまたはmedia/FatFs/state error。 */
mtfs_error_t mtfs_sealed_reader_fatfs_open(
    mtfs_sealed_reader_fatfs_t *context, const TCHAR *path,
    mtfs_sealed_reader_fatfs_media_status_fn media_status,
    void *media_context, mtfs_sealed_reader_t *reader);

/** @brief Close the FatFs file and invalidate published reader callbacks. / FatFs fileをcloseし、返却済みreaderのcallbackを無効化する。
 * @param context Open or initialized adapter. / open済みまたは初期化済みのadapter。
 * @return MTFS_OK or close error. / MTFS_OKまたはclose error。 */
mtfs_error_t mtfs_sealed_reader_fatfs_close(
    mtfs_sealed_reader_fatfs_t *context);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_ENABLE_SEALED_MODEL */

/** @} */

#endif /* MTFS_SEALED_READER_FATFS_H */
