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
    mtfs_error_t status;
    int reloc_status;
    int index = 0;
    int found_activation = 0, found_parameters = 0;
    uint32_t activation_total = 0U;
    if (target == NULL || binary == NULL || actual == NULL ||
        ((uintptr_t)binary & 7U) != 0U) return MTFS_ERROR_INVALID_ARGUMENT;
    target->diagnostic_stage = MTFS_STM32N6_NPU_DIAG_INSPECT_BEGIN;
    target->diagnostic_detail = 0;
    target->diagnostic_expected = 0U;
    target->diagnostic_actual = 0U;
    target->inspected_activation_count = 0U;
    (void)memset(&info, 0, sizeof(info));
    reloc_status = ll_aton_reloc_get_info((uintptr_t)binary, &info);
    if (reloc_status != 0) {
        target->diagnostic_stage = MTFS_STM32N6_NPU_DIAG_RELOC_INFO;
        target->diagnostic_actual = (uint32_t)reloc_status;
        return MTFS_ERROR_MALFORMED_FORMAT;
    }
    if (info.params_off > binary_size || info.params_sz > binary_size - info.params_off) {
        target->diagnostic_stage = MTFS_STM32N6_NPU_DIAG_RELOC_BOUNDS;
        target->diagnostic_expected = binary_size;
        target->diagnostic_actual = info.params_off;
        return MTFS_ERROR_MALFORMED_FORMAT;
    }
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
    actual->regions[0].kind = MTFS_SENTINEL_REGION_EXECUTABLE_COPY;
    actual->regions[0].placement = MTFS_SENTINEL_PLACEMENT_CALLER_RELATIVE;
    actual->regions[0].alignment = 8U;
    actual->regions[0].logical_size = info.rt_ram_copy;
    actual->regions[0].storage_size = info.rt_ram_copy;
    actual->region_count = 1U;
    while ((pool = ll_aton_reloc_get_mem_pool_desc((uintptr_t)binary, index++)) != NULL) {
        mtfs_sentinel_npu_actual_region_t *region;
        if (AI_RELOC_MPOOL_IS_PARAM(pool->flags)) {
            if (found_parameters++) {
                target->diagnostic_stage = MTFS_STM32N6_NPU_DIAG_MEMORY_POOLS;
                target->diagnostic_detail = 1;
                return MTFS_ERROR_MALFORMED_FORMAT;
            }
            if (pool->size != info.params_sz || !AI_RELOC_MPOOL_IS_COPY(pool->flags) ||
                pool->dst == 0U || pool->foff != 0U) {
                target->diagnostic_stage = MTFS_STM32N6_NPU_DIAG_MEMORY_POOLS;
                target->diagnostic_detail = 2;
                target->diagnostic_expected = info.params_sz;
                target->diagnostic_actual = pool->size;
                return MTFS_ERROR_UNSUPPORTED_FORMAT;
            }
            if (actual->region_count >= MTFS_SENTINEL_NPU_MAX_ACTUAL_REGIONS)
                return MTFS_ERROR_UNSUPPORTED_FORMAT;
            region = &actual->regions[actual->region_count++];
            region->kind = MTFS_SENTINEL_REGION_PARAMETERS;
            region->placement = MTFS_SENTINEL_PLACEMENT_FIXED_ABSOLUTE;
            region->alignment = 8U;
            region->logical_size = pool->size;
            region->storage_size = pool->size;
            region->address_or_offset = pool->dst;
        }
        if (AI_RELOC_MPOOL_IS_ACTIV(pool->flags)) {
            if (!AI_RELOC_MPOOL_IS_RESET(pool->flags) || pool->dst == 0U ||
                pool->foff != 0U ||
                actual->region_count >= MTFS_SENTINEL_NPU_MAX_ACTUAL_REGIONS) {
                target->diagnostic_stage = MTFS_STM32N6_NPU_DIAG_MEMORY_POOLS;
                target->diagnostic_detail = 3;
                return MTFS_ERROR_UNSUPPORTED_FORMAT;
            }
            region = &actual->regions[actual->region_count++];
            region->kind = MTFS_SENTINEL_REGION_ACTIVATION;
            region->placement = MTFS_SENTINEL_PLACEMENT_FIXED_ABSOLUTE;
            region->alignment = 8U;
            region->logical_size = pool->size;
            region->storage_size = pool->size;
            region->address_or_offset = pool->dst;
            target->inspected_activation_address[found_activation] = pool->dst;
            target->inspected_activation_size[found_activation] = pool->size;
            ++found_activation;
            if (activation_total > UINT32_MAX - pool->size)
                return MTFS_ERROR_OVERFLOW;
            activation_total += pool->size;
        }
    }
    if (found_parameters != 1 || found_activation == 0 ||
        activation_total != info.acts_sz || info.ext_ram_sz != 0U) {
        target->diagnostic_stage = MTFS_STM32N6_NPU_DIAG_MEMORY_POOLS;
        target->diagnostic_detail = found_parameters != 1 ? 4 :
            (found_activation == 0 ? 5 : 6);
        target->diagnostic_expected = info.acts_sz;
        target->diagnostic_actual = activation_total;
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    }
    target->inspected_activation_count = (uint32_t)found_activation;
    target->diagnostic_stage = MTFS_STM32N6_NPU_DIAG_HASH;
    status = target->hash(target->callback_context, binary, binary_size,
        actual->runtime_binary_hash);
    if (status != MTFS_OK) {
        target->diagnostic_actual = (uint32_t)(int32_t)status;
        return status;
    }
    target->diagnostic_stage = MTFS_STM32N6_NPU_DIAG_INSPECT_COMPLETE;
    return MTFS_OK;
}

static int tensor_contract_matches(const LL_Buffer_InfoTypeDef *info,
    uint32_t dimension, uint32_t scale_numerator, uint32_t scale_shift,
    int8_t zero_point, mtfs_stm32n6_neural_art_t *target)
{
    float expected_scale;
    uint32_t actual_bits, expected_bits;
    uint64_t denominator;
    if (info == NULL) { target->diagnostic_detail = 1; return 0; }
    if (info[0].name == NULL) { target->diagnostic_detail = 2; return 0; }
    if (info[1].name != NULL) { target->diagnostic_detail = 3; return 0; }
    if (info[0].type != DataType_INT8) {
        target->diagnostic_detail = 4; target->diagnostic_expected = DataType_INT8;
        target->diagnostic_actual = info[0].type; return 0;
    }
    if (info[0].nbits != 8U) {
        target->diagnostic_detail = 5; target->diagnostic_expected = 8U;
        target->diagnostic_actual = info[0].nbits; return 0;
    }
    if (info[0].per_channel != 0U) {
        target->diagnostic_detail = 6;
        target->diagnostic_actual = info[0].per_channel; return 0;
    }
    if (LL_Buffer_len(&info[0]) != dimension) {
        target->diagnostic_detail = 7; target->diagnostic_expected = dimension;
        target->diagnostic_actual = LL_Buffer_len(&info[0]); return 0;
    }
    if (info[0].scale == NULL) { target->diagnostic_detail = 8; return 0; }
    if (info[0].offset == NULL) { target->diagnostic_detail = 9; return 0; }
    if (*info[0].offset != zero_point) {
        target->diagnostic_detail = 10;
        target->diagnostic_expected = (uint32_t)(int32_t)zero_point;
        target->diagnostic_actual = (uint32_t)(int32_t)*info[0].offset; return 0;
    }
    if (scale_numerator == 0U) { target->diagnostic_detail = 11; return 0; }
    if (scale_shift > 31U) { target->diagnostic_detail = 12; return 0; }
    denominator = UINT64_C(1) << scale_shift;
    expected_scale = (float)scale_numerator / (float)denominator;
    (void)memcpy(&actual_bits, info[0].scale, sizeof(actual_bits));
    (void)memcpy(&expected_bits, &expected_scale, sizeof(expected_bits));
    if (actual_bits != expected_bits) {
        target->diagnostic_detail = 13;
        target->diagnostic_expected = expected_bits;
        target->diagnostic_actual = actual_bits;
        return 0;
    }
    return 1;
}

static int activation_contains(const mtfs_stm32n6_neural_art_t *target,
    uintptr_t address, uint32_t size)
{
    uint32_t i;
    for (i = 0U; i < target->inspected_activation_count; ++i) {
        uintptr_t start = target->inspected_activation_address[i];
        uint32_t available = target->inspected_activation_size[i];
        if (address >= start && address - start <= available &&
            size <= available - (uint32_t)(address - start)) return 1;
    }
    return 0;
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
    target->diagnostic_stage = MTFS_STM32N6_NPU_DIAG_INSTALL_BEGIN;
    target->diagnostic_detail = 0;
    target->diagnostic_expected = 0U;
    target->diagnostic_actual = 0U;
    (void)memset(&config, 0, sizeof(config));
    config.exec_ram_addr = (uintptr_t)copy_memory;
    config.exec_ram_size = copy_size;
    config.mode = AI_RELOC_RT_LOAD_MODE_COPY;
    target->diagnostic_stage = MTFS_STM32N6_NPU_DIAG_RELOC_INSTALL;
    {
        int install_status = ll_aton_reloc_install((uintptr_t)binary, &config,
            (NN_Instance_TypeDef *)target->nn_instance);
        if (install_status != 0) {
            target->diagnostic_actual = (uint32_t)install_status;
            return MTFS_ERROR_NOT_READY;
        }
    }
    input_info = ll_aton_reloc_get_input_buffers_info(
        (NN_Instance_TypeDef *)target->nn_instance, -1);
    output_info = ll_aton_reloc_get_output_buffers_info(
        (NN_Instance_TypeDef *)target->nn_instance, -1);
    target->diagnostic_stage = MTFS_STM32N6_NPU_DIAG_INPUT_CONTRACT;
    if (!tensor_contract_matches(input_info, runtime->input_dimension,
            runtime->input_scale_numerator, runtime->input_scale_shift,
            runtime->input_zero_point, target))
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    target->diagnostic_stage = MTFS_STM32N6_NPU_DIAG_OUTPUT_CONTRACT;
    if (!tensor_contract_matches(output_info, runtime->output_dimension,
            runtime->output_scale_numerator, runtime->output_scale_shift,
            runtime->output_zero_point, target)) return MTFS_ERROR_UNSUPPORTED_FORMAT;
    target->input = (void *)LL_Buffer_addr_start(&input_info[0]);
    target->output = (void *)LL_Buffer_addr_start(&output_info[0]);
    target->diagnostic_stage = MTFS_STM32N6_NPU_DIAG_BUFFER_ADDRESS;
    if (target->input == NULL || target->output == NULL) {
        target->diagnostic_detail = 1;
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    }
    if (!activation_contains(target, (uintptr_t)target->input,
            LL_Buffer_len(&input_info[0]))) {
        target->diagnostic_detail = 3;
        target->diagnostic_expected = target->inspected_activation_count;
        target->diagnostic_actual = (uint32_t)(uintptr_t)target->input;
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    }
    if (!activation_contains(target, (uintptr_t)target->output,
            LL_Buffer_len(&output_info[0]))) {
        target->diagnostic_detail = 4;
        target->diagnostic_expected = target->inspected_activation_count;
        target->diagnostic_actual = (uint32_t)(uintptr_t)target->output;
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    }
    LL_ATON_RT_RuntimeInit();
    target->runtime_initialized = 1U;
    LL_ATON_RT_Init_Network((NN_Instance_TypeDef *)target->nn_instance);
    target->installed = 1U;
    target->diagnostic_stage = MTFS_STM32N6_NPU_DIAG_INSTALL_COMPLETE;
    return MTFS_OK;
}

static mtfs_error_t infer_runtime(void *opaque, const int8_t input[24],
    int8_t output[24], uint32_t timeout_ms)
{
    mtfs_stm32n6_neural_art_t *target = opaque;
    LL_ATON_RT_RetValues_t state;
    uint64_t start;
    uint32_t stage_start = 0U, total_start = 0U;
    mtfs_error_t status = MTFS_OK;
    int profiling;
    if (!target->installed || timeout_ms == 0U) return MTFS_ERROR_INVALID_STATE;
    profiling = target->profile_cycle_count != NULL;
    if (profiling) {
        ++target->profile.attempted;
        total_start = target->profile_cycle_count(target->profile_cycle_context);
        stage_start = target->profile_cycle_count(target->profile_cycle_context);
    }
    (void)memcpy(target->input, input, 24U);
    if (profiling) {
        target->profile.input_copy_cycles +=
            target->profile_cycle_count(target->profile_cycle_context) -
            stage_start;
        stage_start = target->profile_cycle_count(target->profile_cycle_context);
    }
    LL_ATON_Cache_MCU_Clean_Range((uintptr_t)target->input, 24U);
    if (profiling) {
        target->profile.cache_clean_cycles +=
            target->profile_cycle_count(target->profile_cycle_context) -
            stage_start;
        stage_start = target->profile_cycle_count(target->profile_cycle_context);
    }
    LL_ATON_RT_Reset_Network((NN_Instance_TypeDef *)target->nn_instance);
    if (profiling)
        target->profile.reset_cycles +=
            target->profile_cycle_count(target->profile_cycle_context) -
            stage_start;
    start = target->clock_ms(target->callback_context);
    do {
        if (profiling)
            stage_start = target->profile_cycle_count(
                target->profile_cycle_context);
        state = LL_ATON_RT_RunEpochBlock((NN_Instance_TypeDef *)target->nn_instance);
        if (profiling) {
            target->profile.epoch_cycles +=
                target->profile_cycle_count(target->profile_cycle_context) -
                stage_start;
            ++target->profile.epoch_calls;
            if (state == LL_ATON_RT_NO_WFE) ++target->profile.state_no_wfe;
            else if (state == LL_ATON_RT_WFE) ++target->profile.state_wfe;
            else if (state == LL_ATON_RT_DONE) ++target->profile.state_done;
            else ++target->profile.state_other;
        }
        if (state != LL_ATON_RT_DONE) {
            if (target->clock_ms(target->callback_context) - start >= timeout_ms) {
                status = MTFS_ERROR_NOT_READY;
                goto finish;
            }
            if (profiling)
                stage_start = target->profile_cycle_count(
                    target->profile_cycle_context);
            target->yield(target->callback_context);
            if (profiling) {
                target->profile.wait_cycles +=
                    target->profile_cycle_count(target->profile_cycle_context) -
                    stage_start;
                ++target->profile.yield_calls;
            }
        }
    } while (state != LL_ATON_RT_DONE);
    if (profiling)
        stage_start = target->profile_cycle_count(target->profile_cycle_context);
    LL_ATON_Cache_MCU_Invalidate_Range((uintptr_t)target->output, 24U);
    if (profiling) {
        target->profile.cache_invalidate_cycles +=
            target->profile_cycle_count(target->profile_cycle_context) -
            stage_start;
        stage_start = target->profile_cycle_count(target->profile_cycle_context);
    }
    (void)memcpy(output, target->output, 24U);
    if (profiling) {
        target->profile.output_copy_cycles +=
            target->profile_cycle_count(target->profile_cycle_context) -
            stage_start;
        ++target->profile.completed;
    }
finish:
    if (profiling)
        target->profile.total_cycles +=
            target->profile_cycle_count(target->profile_cycle_context) -
            total_start;
    return status;
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
    uint32_t original_size = size;
    (void)opaque;
    while (size-- != 0U) *bytes++ = 0U;
    LL_ATON_Cache_MCU_Clean_Range((uintptr_t)address, original_size);
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

mtfs_error_t mtfs_stm32n6_neural_art_profile_start(
    mtfs_stm32n6_neural_art_t *target,
    mtfs_sentinel_npu_cycle_count_fn cycle_count, void *cycle_context)
{
    if (target == NULL || cycle_count == NULL || !target->installed)
        return MTFS_ERROR_INVALID_ARGUMENT;
    (void)memset(&target->profile, 0, sizeof(target->profile));
    target->profile_cycle_context = cycle_context;
    target->profile_cycle_count = cycle_count;
    return MTFS_OK;
}

mtfs_error_t mtfs_stm32n6_neural_art_profile_stop(
    mtfs_stm32n6_neural_art_t *target,
    mtfs_stm32n6_neural_art_profile_t *profile)
{
    if (target == NULL || profile == NULL ||
        target->profile_cycle_count == NULL) return MTFS_ERROR_INVALID_STATE;
    *profile = target->profile;
    target->profile_cycle_count = NULL;
    target->profile_cycle_context = NULL;
    return MTFS_OK;
}
