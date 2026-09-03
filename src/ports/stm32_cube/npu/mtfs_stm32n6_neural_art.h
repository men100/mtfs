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
    uint32_t inspected_activation_address;
    uint32_t inspected_activation_size;
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
