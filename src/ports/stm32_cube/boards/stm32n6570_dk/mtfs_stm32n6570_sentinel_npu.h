#ifndef MTFS_STM32N6570_SENTINEL_NPU_H
#define MTFS_STM32N6570_SENTINEL_NPU_H

#include "../../../../sentinel/mtfs_sentinel_inference.h"

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE

#ifdef __cplusplus
extern "C" {
#endif

mtfs_error_t mtfs_stm32n6570_sentinel_npu_policy(
    const mtfs_sentinel_runtime_region_policy_t **policies,
    uint32_t *policy_count);
mtfs_error_t mtfs_stm32n6570_sentinel_npu_linker_policy_validate(void);
mtfs_error_t mtfs_stm32n6570_sentinel_npu_hardware_init(void);

#ifdef __cplusplus
}
#endif
#endif
#endif
