#ifndef MTFS_RA8P1_SENTINEL_NPU_H
#define MTFS_RA8P1_SENTINEL_NPU_H

#include "../../../../sentinel/mtfs_sentinel_inference.h"

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_RA8P1_SENTINEL_RUNTIME_BINARY_SIZE (UINT32_C(3392))

mtfs_error_t mtfs_ra8p1_sentinel_npu_policy(
    const mtfs_sentinel_runtime_region_policy_t **policies,
    uint32_t *policy_count);

#ifdef __cplusplus
}
#endif
#endif
#endif
