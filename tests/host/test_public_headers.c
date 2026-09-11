#include "mtfs.h"
#include "core/mtfs_media.h"
#include "sentinel/mtfs_sentinel_npu_provider.h"
#include "sentinel/mtfs_sentinel_baseline.h"
#include "sentinel/mtfs_sentinel_sha256.h"
#include "mtfs_sentinel_monitor.h"

_Static_assert(MTFS_BLOCK_DIAGNOSTICS_API_VERSION == 1U,
    "diagnostics API version must be visible from C");
_Static_assert(MTFS_SENTINEL_SCHEMA_VERSION == 1U,
    "Sentinel schema must be visible from C");
_Static_assert(MTFS_SENTINEL_BUNDLE_VERSION == 1U,
    "Sentinel bundle API must be visible from C");
_Static_assert(MTFS_SENTINEL_NPU_PROVIDER_API_VERSION == 1U,
    "Sentinel NPU provider API must be visible from C");
_Static_assert(MTFS_SENTINEL_BASELINE_PREPROCESSING_VERSION == 2U,
    "baseline-relative preprocessing contract must be visible from C");
_Static_assert(MTFS_MODEL_FORMAT_SENTINEL_BUNDLE_V1 == UINT32_C(0x534e5431),
    "sealed outer model format registry ID");
