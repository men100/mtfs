#include "mtfs.h"
#include "core/mtfs_media.h"

static_assert(MTFS_BLOCK_DIAGNOSTICS_API_VERSION == 1U,
    "diagnostics API version must be visible from C++");
static_assert(MTFS_SENTINEL_SCHEMA_VERSION == 1U,
    "Sentinel schema must be visible from C++");
