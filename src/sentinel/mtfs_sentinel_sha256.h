/** @file mtfs_sentinel_sha256.h
 * @brief Portable SHA-256 helper for runtime identity. / 実行時の識別用に使う、ポータブルな SHA-256 ヘルパー。
 * @ingroup mtfs_sentinel */
#ifndef MTFS_SENTINEL_SHA256_H
#define MTFS_SENTINEL_SHA256_H

/** @addtogroup mtfs_sentinel
 * @{ */

#include "../mtfs_config.h"

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE
#include <stddef.h>
#include <stdint.h>
#include "../mtfs_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Hash an in-memory byte sequence. / メモリ上のバイト列の SHA-256 ハッシュを計算する。
 * @param bytes Input, or NULL only when size is zero. / 入力データ。size が 0 の場合に限り NULL を指定可能。
 * @param size Input bytes. / 入力データのサイズ（バイト単位）
 * @param[out] digest Exactly 32 output bytes. / 32バイトのハッシュ値出力先
 * @return MTFS_OK or INVALID_ARGUMENT. / MTFS_OKまたはINVALID_ARGUMENT。
 * @note This is an integrity comparison helper, not keyed authentication. / データの同一性・完全性確認用のヘルパーであり、
 *       HMAC のような秘密鍵を用いた認証機能は提供しない。 */
mtfs_error_t mtfs_sentinel_sha256(const void *bytes, size_t size,
    uint8_t digest[32]);

#ifdef __cplusplus
}
#endif

/** @} */
#endif
#endif
