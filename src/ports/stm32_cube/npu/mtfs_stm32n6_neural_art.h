#ifndef MTFS_STM32N6_NEURAL_ART_H
#define MTFS_STM32N6_NEURAL_ART_H

#include "../../../sentinel/mtfs_sentinel_npu_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef mtfs_error_t (*mtfs_stm32n6_npu_hash_fn)(void *context,
    const void *bytes, uint32_t size, uint8_t digest[32]);
typedef uint64_t (*mtfs_stm32n6_npu_clock_fn)(void *context);
typedef void (*mtfs_stm32n6_npu_yield_fn)(void *context);

/* Diagnostic substages are observational only and are not part of policy. */
#define MTFS_STM32N6_NPU_DIAG_INSPECT_BEGIN       (UINT32_C(1))
#define MTFS_STM32N6_NPU_DIAG_RELOC_INFO          (UINT32_C(2))
#define MTFS_STM32N6_NPU_DIAG_RELOC_BOUNDS        (UINT32_C(3))
#define MTFS_STM32N6_NPU_DIAG_MEMORY_POOLS        (UINT32_C(4))
#define MTFS_STM32N6_NPU_DIAG_HASH                (UINT32_C(5))
#define MTFS_STM32N6_NPU_DIAG_INSPECT_COMPLETE    (UINT32_C(10))
#define MTFS_STM32N6_NPU_DIAG_INSTALL_BEGIN       (UINT32_C(20))
#define MTFS_STM32N6_NPU_DIAG_RELOC_INSTALL       (UINT32_C(21))
#define MTFS_STM32N6_NPU_DIAG_INPUT_CONTRACT      (UINT32_C(22))
#define MTFS_STM32N6_NPU_DIAG_OUTPUT_CONTRACT     (UINT32_C(23))
#define MTFS_STM32N6_NPU_DIAG_BUFFER_ADDRESS      (UINT32_C(24))
#define MTFS_STM32N6_NPU_DIAG_INSTALL_COMPLETE    (UINT32_C(30))

typedef struct mtfs_stm32n6_neural_art
{
    void *nn_instance;
    void *input;
    void *output;
    mtfs_stm32n6_npu_hash_fn hash;
    mtfs_stm32n6_npu_clock_fn clock_ms;
    mtfs_stm32n6_npu_yield_fn yield;
    mtfs_sentinel_npu_lock_fn lock;
    mtfs_sentinel_npu_unlock_fn unlock;
    void *callback_context;
    uint8_t installed;
    uint8_t runtime_initialized;
    uint16_t reserved;
    uint32_t inspected_activation_address[MTFS_SENTINEL_NPU_MAX_ACTUAL_REGIONS];
    uint32_t inspected_activation_size[MTFS_SENTINEL_NPU_MAX_ACTUAL_REGIONS];
    uint32_t inspected_activation_count;
    uint32_t diagnostic_stage;
    int32_t diagnostic_detail;
    uint32_t diagnostic_expected;
    uint32_t diagnostic_actual;
} mtfs_stm32n6_neural_art_t;

mtfs_error_t mtfs_stm32n6_neural_art_provider_config(
    mtfs_stm32n6_neural_art_t *target, void *nn_instance,
    mtfs_stm32n6_npu_hash_fn hash, mtfs_stm32n6_npu_clock_fn clock_ms,
    mtfs_stm32n6_npu_yield_fn yield, mtfs_sentinel_npu_lock_fn lock,
    mtfs_sentinel_npu_unlock_fn unlock, void *callback_context,
    mtfs_sentinel_npu_provider_config_t *config);

#ifdef __cplusplus
}
#endif
#endif
