/** @file mtfs_secure_zero.h
 * @brief Non-elidable sensitive-memory clearing. / compiler最適化で除去されないsensitive memory消去。
 * @ingroup mtfs_sealed */
#ifndef MTFS_SECURE_ZERO_H
#define MTFS_SECURE_ZERO_H

/** @addtogroup mtfs_sealed
 * @{ */

#include "mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Overwrite a caller-selected region with zero. / 呼び出し側が指定した領域をzeroで上書きする。
 * @param data Writable region; may be NULL only when size is zero. / 書き込み可能な領域。sizeが0の場合のみNULL指定可。
 * @param size Bytes to clear. / 消去するサイズ（byte単位）。
 * @post Exactly the supplied region is cleared; adjacent storage is untouched. / 指定された領域だけを消去し、隣接する領域は変更しない。 */
void mtfs_secure_zero(void *data, size_t size);

#ifdef __cplusplus
}
#endif
#endif /* MTFS_ENABLE_SEALED_MODEL */

/** @} */

#endif /* MTFS_SECURE_ZERO_H */
