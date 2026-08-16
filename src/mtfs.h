/*
 * microT-FS public umbrella header.
 * The public API will be introduced with the first implementation milestone.
 */
#ifndef MTFS_H
#define MTFS_H

#include "mtfs_config.h"
#include "mtfs_error.h"
#include "mtfs_types.h"
#include "block/mtfs_block_device.h"
#include "block/mtfs_block_diagnostics.h"
#include "block/mtfs_block_registry.h"
#include "core/mtfs_media.h"
#include "core/mtfs_time.h"

#define MTFS_VERSION_MAJOR (0U)
#define MTFS_VERSION_MINOR (1U)
#define MTFS_VERSION_PATCH (0U)

#endif /* MTFS_H */
