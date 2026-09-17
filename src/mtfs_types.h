/** @file mtfs_types.h
 * @brief Common public types. / 共通の公開型定義。
 * @ingroup mtfs_core */
#ifndef MTFS_TYPES_H
#define MTFS_TYPES_H

/** @addtogroup mtfs_core
 * @{ */

#include <stdint.h>

typedef uint64_t mtfs_lba_t;

/* Public object definitions remain private until the core API is established. */
/* core APIが確定するまでは、public objectの完全な定義は公開しない。 */
typedef struct mtfs_context mtfs_context_t;
typedef struct mtfs_block_device mtfs_block_device_t;

/** @} */

#endif /* MTFS_TYPES_H */
