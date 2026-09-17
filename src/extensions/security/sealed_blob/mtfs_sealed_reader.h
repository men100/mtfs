/** @file mtfs_sealed_reader.h
 * @brief Random-access source for sealed packages. / sealed packageをrandom accessで読み取るためのsource interface。
 * @ingroup mtfs_sealed */
#ifndef MTFS_SEALED_READER_H
#define MTFS_SEALED_READER_H

/** @addtogroup mtfs_sealed
 * @{ */

#include "mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL
#include <stddef.h>
#include <stdint.h>
#include "mtfs_error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_SEALED_READER_API_VERSION (1U)

/** @brief Caller-owned reader callback table. / 呼び出し側が所有するreader callback table。
 * @details read_at reports the exact byte count through read_size; short reads are handled as input failure by sealed-blob loading. The reader and context must remain valid until the blob is closed.
 * / read_atは実際に読み取ったbyte数をread_sizeに返す。要求サイズより短いreadはsealed blobのload時に入力errorとして扱われる。blobをcloseするまでreaderとcontextは有効な状態を維持する必要がある。
 * @note Callbacks run in task context and ownership never transfers. / callbackはtask contextで実行され、readerやcontextの所有権は移動しない。 */
typedef struct mtfs_sealed_reader
{
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    mtfs_error_t (*get_size)(void *context, uint64_t *size);
    mtfs_error_t (*read_at)(void *context, uint64_t offset, void *buffer,
                            size_t requested, size_t *read_size);
} mtfs_sealed_reader_t;

#ifdef __cplusplus
}
#endif
#endif /* MTFS_ENABLE_SEALED_MODEL */

/** @} */

#endif /* MTFS_SEALED_READER_H */
