#include "mtfs_stm32n6_neural_art.h"

#include <string.h>

#include "ll_aton_caches_interface.h"
#include "ll_aton_reloc_network.h"
#include "ll_aton_rt_user_api.h"

static mtfs_error_t inspect_runtime(void *opaque, const uint8_t *binary,
    uint32_t binary_size, mtfs_sentinel_npu_actual_info_t *actual)
{
    mtfs_stm32n6_neural_art_t *target = opaque;
    ll_aton_reloc_info info;
    ll_aton_reloc_mem_pool_desc *pool;
    int index = 0;
    int found_activation = 0, found_parameters = 0;
    if (target == NULL || binary == NULL || actual == NULL ||
        ((uintptr_t)binary & 7U) != 0U) return MTFS_ERROR_INVALID_ARGUMENT;
    (void)memset(&info, 0, sizeof(info));
    if (ll_aton_reloc_get_info((uintptr_t)binary, &info) != 0)
        return MTFS_ERROR_MALFORMED_FORMAT;
    if (info.params_off > binary_size || info.params_sz > binary_size - info.params_off)
        return MTFS_ERROR_MALFORMED_FORMAT;
    (void)memset(actual, 0, sizeof(*actual));
    actual->runtime_abi = ((uint32_t)AI_RELOC_RT_GET_MAJOR(info.variant) << 16U) |
        (uint32_t)AI_RELOC_RT_GET_MINOR(info.variant);
    actual->runtime_variant = info.variant;
    actual->runtime_extra = info.rt_version_extra;
    actual->copy_size = info.rt_ram_copy;
    actual->copy_alignment = 8U;
    actual->parameters_offset = info.params_off;
    actual->parameters_logical_size = info.params_sz;
    actual->parameters_storage_size = binary_size - info.params_off;
    actual->external_ram_size = info.ext_ram_sz;
    while ((pool = ll_aton_reloc_get_mem_pool_desc((uintptr_t)binary, index++)) != NULL) {
        if (AI_RELOC_MPOOL_IS_PARAM(pool->flags)) {
            if (found_parameters++) return MTFS_ERROR_MALFORMED_FORMAT;
            if (pool->size != info.params_sz) return MTFS_ERROR_UNSUPPORTED_FORMAT;
        }
        if (AI_RELOC_MPOOL_IS_ACTIV(pool->flags)) {
            if (found_activation++) return MTFS_ERROR_MALFORMED_FORMAT;
            actual->activation_address = pool->dst;
            actual->activation_size = pool->size;
        }
    }
    if (found_parameters != 1 || found_activation != 1 ||
        actual->activation_size != info.acts_sz)
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    target->inspected_activation_address = actual->activation_address;
    target->inspected_activation_size = actual->activation_size;
    return target->hash(target->callback_context, binary, binary_size,
                        actual->runtime_binary_hash);
}

static int tensor_contract_matches(const LL_Buffer_InfoTypeDef *info,
    uint32_t dimension, uint32_t scale_numerator, uint32_t scale_shift,
    int8_t zero_point)
{
    float expected_scale;
    uint32_t actual_bits, expected_bits;
    uint64_t denominator;
    if (info == NULL || info[0].name == NULL || info[1].name != NULL ||
        info[0].type != DataType_INT8 || info[0].nbits != 8U ||
        info[0].per_channel != 0U || LL_Buffer_len(&info[0]) != dimension ||
        info[0].scale == NULL || info[0].offset == NULL ||
        *info[0].offset != zero_point || scale_numerator == 0U ||
        scale_shift > 31U) return 0;
    denominator = UINT64_C(1) << scale_shift;
    expected_scale = (float)scale_numerator / (float)denominator;
    (void)memcpy(&actual_bits, info[0].scale, sizeof(actual_bits));
    (void)memcpy(&expected_bits, &expected_scale, sizeof(expected_bits));
    return actual_bits == expected_bits;
}

static mtfs_error_t install_runtime(void *opaque, const uint8_t *binary,
    uint32_t binary_size, void *copy_memory, uint32_t copy_size,
    const mtfs_sentinel_runtime_info_t *runtime)
{
    mtfs_stm32n6_neural_art_t *target = opaque;
    ll_aton_reloc_config config;
    const LL_Buffer_InfoTypeDef *input_info, *output_info;
    (void)binary_size;
    if (target == NULL || binary == NULL || copy_memory == NULL ||
        runtime == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    (void)memset(&config, 0, sizeof(config));
    config.exec_ram_addr = (uintptr_t)copy_memory;
    config.exec_ram_size = copy_size;
    config.mode = AI_RELOC_RT_LOAD_MODE_COPY;
    if (ll_aton_reloc_install((uintptr_t)binary, &config,
            (NN_Instance_TypeDef *)target->nn_instance) != 0)
        return MTFS_ERROR_NOT_READY;
    input_info = ll_aton_reloc_get_input_buffers_info(
        (NN_Instance_TypeDef *)target->nn_instance, -1);
    output_info = ll_aton_reloc_get_output_buffers_info(
        (NN_Instance_TypeDef *)target->nn_instance, -1);
    if (!tensor_contract_matches(input_info, runtime->input_dimension,
            runtime->input_scale_numerator, runtime->input_scale_shift,
            runtime->input_zero_point) ||
        !tensor_contract_matches(output_info, runtime->output_dimension,
            runtime->output_scale_numerator, runtime->output_scale_shift,
            runtime->output_zero_point)) return MTFS_ERROR_UNSUPPORTED_FORMAT;
    target->input = (void *)LL_Buffer_addr_start(&input_info[0]);
    target->output = (void *)LL_Buffer_addr_start(&output_info[0]);
    if (target->input == NULL || target->output == NULL ||
        target->input != target->output ||
        (uintptr_t)target->input != target->inspected_activation_address ||
        LL_Buffer_len(&input_info[0]) > target->inspected_activation_size ||
        LL_Buffer_len(&output_info[0]) > target->inspected_activation_size)
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    LL_ATON_RT_RuntimeInit();
    target->runtime_initialized = 1U;
    LL_ATON_RT_Init_Network((NN_Instance_TypeDef *)target->nn_instance);
    target->installed = 1U;
    return MTFS_OK;
}

static mtfs_error_t infer_runtime(void *opaque, const int8_t input[24],
    int8_t output[24], uint32_t timeout_ms)
{
    mtfs_stm32n6_neural_art_t *target = opaque;
    LL_ATON_RT_RetValues_t state;
    uint64_t start;
    if (!target->installed || timeout_ms == 0U) return MTFS_ERROR_INVALID_STATE;
    (void)memcpy(target->input, input, 24U);
    LL_ATON_Cache_MCU_Clean_Range((uintptr_t)target->input, 24U);
    LL_ATON_RT_Reset_Network((NN_Instance_TypeDef *)target->nn_instance);
    start = target->clock_ms(target->callback_context);
    do {
        state = LL_ATON_RT_RunEpochBlock((NN_Instance_TypeDef *)target->nn_instance);
        if (state != LL_ATON_RT_DONE) {
            if (target->clock_ms(target->callback_context) - start >= timeout_ms)
                return MTFS_ERROR_NOT_READY;
            target->yield(target->callback_context);
        }
    } while (state != LL_ATON_RT_DONE);
    LL_ATON_Cache_MCU_Invalidate_Range((uintptr_t)target->output, 24U);
    (void)memcpy(output, target->output, 24U);
    return MTFS_OK;
}

static mtfs_error_t close_runtime(void *opaque)
{
    mtfs_stm32n6_neural_art_t *target = opaque;
    if (!target->installed) return MTFS_ERROR_INVALID_STATE;
    LL_ATON_RT_DeInit_Network((NN_Instance_TypeDef *)target->nn_instance);
    target->installed = 0U;
    if (target->runtime_initialized) {
        LL_ATON_RT_RuntimeDeInit();
        target->runtime_initialized = 0U;
    }
    target->input = NULL; target->output = NULL;
    return MTFS_OK;
}

static mtfs_error_t lock_runtime(void *opaque, uint32_t timeout_ms)
{
    mtfs_stm32n6_neural_art_t *target = opaque;
    return target->lock(target->callback_context, timeout_ms);
}

static void unlock_runtime(void *opaque)
{
    mtfs_stm32n6_neural_art_t *target = opaque;
    target->unlock(target->callback_context);
}

static void zeroize_runtime(void *opaque, void *address, uint32_t size)
{
    volatile uint8_t *bytes = address;
    (void)opaque;
    while (size-- != 0U) *bytes++ = 0U;
}

mtfs_error_t mtfs_stm32n6_neural_art_provider_config(
    mtfs_stm32n6_neural_art_t *target, void *nn_instance,
    mtfs_stm32n6_npu_hash_fn hash, mtfs_stm32n6_npu_clock_fn clock_ms,
    mtfs_stm32n6_npu_yield_fn yield, mtfs_sentinel_npu_lock_fn lock,
    mtfs_sentinel_npu_unlock_fn unlock, void *callback_context,
    mtfs_sentinel_npu_provider_config_t *config)
{
    static const mtfs_sentinel_npu_provider_ops_t ops = {
        inspect_runtime, install_runtime, infer_runtime, close_runtime,
        lock_runtime, unlock_runtime, zeroize_runtime
    };
    if (target == NULL || nn_instance == NULL || hash == NULL || clock_ms == NULL ||
        yield == NULL || lock == NULL || unlock == NULL || config == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    (void)memset(target, 0, sizeof(*target));
    target->nn_instance = nn_instance; target->hash = hash;
    target->clock_ms = clock_ms; target->yield = yield;
    target->lock = lock; target->unlock = unlock;
    target->callback_context = callback_context;
    (void)memset(config, 0, sizeof(*config));
    config->api_version = MTFS_SENTINEL_NPU_PROVIDER_API_VERSION;
    config->struct_size = (uint16_t)sizeof(*config);
    config->provider_id = MTFS_SENTINEL_PROVIDER_ST_NEURAL_ART_RELOC;
    config->accelerator_id = MTFS_SENTINEL_ACCELERATOR_NEURAL_ART;
    config->ops = &ops; config->target = target;
    return MTFS_OK;
}
