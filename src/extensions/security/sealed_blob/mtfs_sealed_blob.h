/** @file mtfs_sealed_blob.h
 * @brief Authenticated chunked sealed-blob loader. / 認証付きchunked sealed blobのloader。
 * @ingroup mtfs_sealed */
#ifndef MTFS_SEALED_BLOB_H
#define MTFS_SEALED_BLOB_H

/** @addtogroup mtfs_sealed
 * @{ */

#include "mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL
#include <stddef.h>
#include <stdint.h>
#include "mtfs_crypto_provider.h"
#include "mtfs_sealed_format.h"
#include "mtfs_sealed_reader.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_SEALED_BLOB_API_VERSION (1U)
#define MTFS_SEALED_WORK_API_VERSION (1U)

typedef enum mtfs_sealed_blob_state
{
    MTFS_SEALED_BLOB_CLOSED = 0,
    MTFS_SEALED_BLOB_OPENING = 1,
    MTFS_SEALED_BLOB_OPEN = 2,
    MTFS_SEALED_BLOB_LOADING = 3,
    MTFS_SEALED_BLOB_LOADED = 4,
    MTFS_SEALED_BLOB_ERROR = 5
} mtfs_sealed_blob_state_t;

typedef struct mtfs_sealed_work
{
    uint32_t api_version;
    uint32_t struct_size;
    uint8_t *manifest;
    size_t manifest_capacity;
    uint8_t *aad;
    size_t aad_capacity;
    uint8_t *ciphertext;
    size_t ciphertext_capacity;
    uint8_t *plaintext;
    size_t plaintext_capacity;
} mtfs_sealed_work_t;

/*
 * Caller-owned work buffers are reused during authenticated loading and may temporarily contain plaintext that must be treated as secret.
 * 認証付きload処理では、呼び出し側が所有するwork bufferを再利用する。work bufferには一時的にplaintextが格納される可能性があるため、秘密情報として扱う必要がある。
 */

typedef struct mtfs_sealed_blob
{
    uint32_t api_version;
    uint32_t struct_size;
    mtfs_sealed_blob_state_t state;
    mtfs_sealed_reader_t reader;
    mtfs_crypto_provider_t provider;
    mtfs_sealed_work_t work;
    mtfs_sealed_package_info_t info;
    mtfs_sealed_layout_t layout;
    mtfs_crypto_key_handle_t fleet_handle;
    mtfs_crypto_key_handle_t model_handle;
    uint64_t file_size;
} mtfs_sealed_blob_t;

/** @brief Initialize a caller-owned closed object. / 呼び出し側が所有するobjectをCLOSED状態で初期化する。
 * @param blob Storage to initialize; no resources are acquired. / 初期化対象のstorage。この処理ではresourceを取得しない。
 * @post blob is CLOSED and safe to pass to open/close. / blobはCLOSED状態となり、open/closeへ安全に渡せる。 */
void mtfs_sealed_blob_init(mtfs_sealed_blob_t *blob);

/** @brief Parse, authenticate, and open a sealed package. / sealed packageをparse・認証し、openする。
 * @param blob Initialized object. / 初期化済みobject。
 * @param reader Borrowed random-access reader retained until close. / closeまで有効な状態を維持する非所有のrandom-access reader。
 * @param provider Borrowed crypto provider retained until close. / closeまで有効な状態を維持する非所有のcrypto provider。
 * @param work Caller-owned buffers retained and reused until close. / closeまで有効な状態を維持し、処理中に再利用される呼び出し側所有のwork buffer。
 * @param[out] authenticated_info Package info valid only after successful authentication. / 認証成功後にのみ有効となるpackage infoの出力先。
 * @return MTFS_OK or reader/format/authentication/crypto/state error. / MTFS_OKまたはreader/format/認証/crypto/state error。
 * @warning work buffers may contain plaintext and are zeroized only within the ranges described by their capacities. / work bufferにはplaintextが格納される可能性があり、zeroizeされるのは各capacityで指定された範囲内のみ。 */
mtfs_error_t mtfs_sealed_blob_open(mtfs_sealed_blob_t *blob,
    const mtfs_sealed_reader_t *reader, const mtfs_crypto_provider_t *provider,
    const mtfs_sealed_work_t *work, mtfs_sealed_package_info_t *authenticated_info);

/** @brief Authenticate every chunk and load plaintext into caller RAM. / すべてのchunkを認証し、plaintextを呼び出し側のRAMへloadする。
 * @param blob Successfully opened blob. / openに成功したblob。
 * @param[out] destination Caller-owned output with no forbidden overlap with work buffers. / 呼び出し側が所有する出力領域。work bufferと禁止されている領域重複があってはならない。
 * @param destination_size Available bytes. / destinationの利用可能サイズ（byte単位）。
 * @param[out] loaded_size Set to authenticated payload bytes on success. / 成功時に認証済みpayloadのサイズ（byte単位）を返す。
 * @return MTFS_OK or size/I/O/authentication/crypto/state error. / MTFS_OKまたはsize/I/O/認証/crypto/state error。
 * @post On failure, produced plaintext in destination and temporary plaintext work ranges is zeroized; the blob enters ERROR until close. / 失敗時は、destinationへ出力済みのplaintextとwork buffer内の一時plaintext領域をzeroizeする。blobはcloseされるまでERROR状態となる。 */
mtfs_error_t mtfs_sealed_blob_load(mtfs_sealed_blob_t *blob,
    void *destination, size_t destination_size, size_t *loaded_size);

/** @brief Release provider key handles and clear sensitive object state. / providerのkey handleを解放し、object内の機密情報を消去する。
 * @param blob Initialized, open, loaded, or error object. / 初期化済み、OPEN、LOADED、またはERROR状態のobject。
 * @return MTFS_OK or the first key-close error. / MTFS_OK、または最初に発生したkey close error。
 * @post Internal handles/work ranges are cleared and the object is CLOSED; caller-owned destination RAM is not owned by this object after a successful load. / internal handleとwork領域を消去し、objectをCLOSED状態にする。load成功後のdestination RAMは呼び出し側が所有し、このobjectでは管理しない。 */
mtfs_error_t mtfs_sealed_blob_close(mtfs_sealed_blob_t *blob);

#ifdef __cplusplus
}
#endif
#endif /* MTFS_ENABLE_SEALED_MODEL */

/** @} */

#endif /* MTFS_SEALED_BLOB_H */
