/**
 * @file mtfs.h
 * @brief Public umbrella header for portable microT-FS APIs. / portableなmicroT-FS公開APIをまとめて提供するumbrella header。
 * @details Optional declarations follow the feature macros in mtfs_config.h. target-port and OS-adapter headers are included separately because they add platform dependencies.
 * / optionalなAPI宣言はmtfs_config.hのfeature macro設定に従う。platform依存を追加するtarget portおよびOS adapterのheaderは、必要に応じて個別にincludeする。
 * @ingroup mtfs_core
 */
#ifndef MTFS_H
#define MTFS_H

/** @addtogroup mtfs_core
 * @{ */

#include "mtfs_config.h"
#include "mtfs_error.h"
#include "mtfs_types.h"

#if MTFS_ENABLE_SEALED_MODEL
#include "extensions/ai/model_store/mtfs_model_store.h"
#include "extensions/security/sealed_blob/mtfs_crypto_provider.h"
#include "extensions/security/sealed_blob/mtfs_sealed_blob.h"
#include "extensions/security/sealed_blob/mtfs_sealed_format.h"
#include "extensions/security/sealed_blob/mtfs_sealed_reader.h"
#include "extensions/security/sealed_blob/mtfs_secure_zero.h"
#endif
#include "block/mtfs_block_device.h"
#include "block/mtfs_block_diagnostics.h"
#include "block/mtfs_block_registry.h"
#if MTFS_ENABLE_STORAGE_SENTINEL
#include "sentinel/mtfs_sentinel.h"
#include "sentinel/mtfs_sentinel_baseline.h"
#include "sentinel/mtfs_sentinel_observer.h"
#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE
#include "sentinel/mtfs_sentinel_inference.h"
#if MTFS_ENABLE_SEALED_MODEL
#include "sentinel/mtfs_sentinel_sealed_adapter.h"
#endif
#endif
#endif
#include "core/mtfs_media.h"
#include "core/mtfs_time.h"

#define MTFS_VERSION_MAJOR (0U)
#define MTFS_VERSION_MINOR (1U)
#define MTFS_VERSION_PATCH (0U)

/** @} */

#endif /* MTFS_H */
