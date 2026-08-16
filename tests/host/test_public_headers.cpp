#include "mtfs.h"
#include "core/mtfs_media.h"

static_assert(MTFS_BLOCK_DIAGNOSTICS_API_VERSION == 1U,
    "diagnostics API version must be visible from C++");
