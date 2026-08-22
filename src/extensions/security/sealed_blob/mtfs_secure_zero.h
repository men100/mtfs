#ifndef MTFS_SECURE_ZERO_H
#define MTFS_SECURE_ZERO_H

#include "mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void mtfs_secure_zero(void *data, size_t size);

#ifdef __cplusplus
}
#endif
#endif /* MTFS_ENABLE_SEALED_MODEL */

#endif /* MTFS_SECURE_ZERO_H */
