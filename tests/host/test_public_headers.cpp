#include "mtfs.h"
#include "core/mtfs_media.h"

static_assert(MTFS_BLOCK_DIAGNOSTICS_API_VERSION == 1U,
    "diagnostics API version must be visible from C++");
static_assert(MTFS_SENTINEL_SCHEMA_VERSION == 1U,
    "Sentinel schema must be visible from C++");
static_assert(MTFS_SENTINEL_BUNDLE_VERSION == 1U,
    "Sentinel bundle API must be visible from C++");
#if UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(mtfs_sentinel_bundle_policy_t) == 28U,
    "32-bit bundle policy ABI size");
static_assert(sizeof(mtfs_sentinel_bundle_t) == 352U,
    "32-bit parsed bundle ABI size");
static_assert(sizeof(mtfs_sentinel_cpu_context_t) == 32U,
    "32-bit CPU context ABI size");
static_assert(sizeof(mtfs_sentinel_inference_result_t) == 32U,
    "32-bit inference result ABI size");
static_assert(sizeof(mtfs_sentinel_runtime_info_t) == 168U,
    "32-bit runtime info ABI size");
static_assert(sizeof(mtfs_sentinel_memory_plan_t) == 64U,
    "32-bit memory plan ABI size");
#endif
static_assert(MTFS_MODEL_FORMAT_SENTINEL_BUNDLE_V1 == 0x534e5431U,
    "sealed outer model format registry ID");
