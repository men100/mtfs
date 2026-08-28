/*
 * microT-FS public umbrella header.
 * The public API will be introduced with the first implementation milestone.
 */
#ifndef MTFS_H
#define MTFS_H

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
#include "sentinel/mtfs_sentinel_observer.h"
#endif
#include "core/mtfs_media.h"
#include "core/mtfs_time.h"

#define MTFS_VERSION_MAJOR (0U)
#define MTFS_VERSION_MINOR (1U)
#define MTFS_VERSION_PATCH (0U)

#endif /* MTFS_H */
