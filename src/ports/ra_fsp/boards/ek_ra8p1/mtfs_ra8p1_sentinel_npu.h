#ifndef MTFS_RA8P1_SENTINEL_NPU_H
#define MTFS_RA8P1_SENTINEL_NPU_H

#include "../../../../sentinel/mtfs_sentinel_inference.h"

#ifdef __cplusplus
extern "C" {
#endif

mtfs_error_t mtfs_ra8p1_sentinel_npu_policy(
    const mtfs_sentinel_runtime_region_policy_t **policies,
    uint32_t *policy_count);

#ifdef __cplusplus
}
#endif
#endif
