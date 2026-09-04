#include "mtfs_sentinel_npu_provider.h"

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE

#include <limits.h>
#include <string.h>

static int8_t saturate_i8(int64_t value)
{
    if (value > 127) return 127;
    if (value < -128) return -128;
    return (int8_t)value;
}

static int64_t divide_round_away(int64_t numerator, uint64_t denominator)
{
    uint64_t magnitude = numerator < 0 ? (uint64_t)(-(numerator + 1)) + 1U :
        (uint64_t)numerator;
    uint64_t quotient = magnitude / denominator;
    uint64_t remainder = magnitude % denominator;
    if (remainder >= (denominator + 1U) / 2U) ++quotient;
    return numerator < 0 ? -(int64_t)quotient : (int64_t)quotient;
}

mtfs_error_t mtfs_sentinel_requantize_q4_to_int8(int8_t input_q4,
    uint32_t scale_numerator, uint32_t scale_shift, int8_t zero_point,
    int8_t *output)
{
    int64_t numerator, quantized;
    uint64_t denominator;
    if (output == NULL || scale_numerator == 0U || scale_shift > 31U)
        return MTFS_ERROR_INVALID_ARGUMENT;
    numerator = (int64_t)input_q4 * ((int64_t)UINT32_C(1) << scale_shift);
    denominator = (uint64_t)scale_numerator * UINT64_C(16);
    quantized = divide_round_away(numerator, denominator) + zero_point;
    *output = saturate_i8(quantized);
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_requantize_int8_to_q4(int8_t input,
    uint32_t scale_numerator, uint32_t scale_shift, int8_t zero_point,
    int8_t *output_q4)
{
    int64_t centered, numerator;
    if (output_q4 == NULL || scale_numerator == 0U || scale_shift > 31U)
        return MTFS_ERROR_INVALID_ARGUMENT;
    centered = (int64_t)input - zero_point;
    numerator = centered * (int64_t)scale_numerator * INT64_C(16);
    *output_q4 = saturate_i8(divide_round_away(numerator,
        UINT64_C(1) << scale_shift));
    return MTFS_OK;
}

static mtfs_error_t compare_actual(const mtfs_sentinel_bundle_t *bundle,
    uint32_t runtime_index, const mtfs_sentinel_runtime_info_t *runtime,
    const mtfs_sentinel_npu_actual_info_t *actual)
{
    mtfs_sentinel_runtime_region_info_t region;
    uint32_t i;
    int copy = 0, activation = 0, parameters = 0, external = 0;
    if (runtime->runtime_abi != actual->runtime_abi ||
        runtime->runtime_variant != actual->runtime_variant ||
        runtime->runtime_extra != actual->runtime_extra ||
        memcmp(runtime->runtime_binary_hash, actual->runtime_binary_hash, 32U) != 0)
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    if (actual->region_count != 0U) {
        uint32_t matched_mask = 0U;
        if (actual->region_count != runtime->region_count ||
            actual->region_count > MTFS_SENTINEL_NPU_MAX_ACTUAL_REGIONS)
            return MTFS_ERROR_UNSUPPORTED_FORMAT;
        for (i = 0U; i < runtime->region_count; ++i) {
            uint32_t j, matches = 0U, match_index = 0U;
            mtfs_error_t status = mtfs_sentinel_bundle_runtime_region_get(bundle,
                runtime_index, i, &region);
            if (status != MTFS_OK) return status;
            for (j = 0U; j < actual->region_count; ++j) {
                const mtfs_sentinel_npu_actual_region_t *candidate =
                    &actual->regions[j];
                if ((matched_mask & (UINT32_C(1) << j)) == 0U &&
                    region.kind == candidate->kind &&
                    region.placement == candidate->placement &&
                    region.alignment == candidate->alignment &&
                    region.logical_size == candidate->logical_size &&
                    region.storage_size == candidate->storage_size &&
                    region.address_or_offset == candidate->address_or_offset) {
                    ++matches;
                    match_index = j;
                }
            }
            if (matches != 1U) return MTFS_ERROR_UNSUPPORTED_FORMAT;
            matched_mask |= UINT32_C(1) << match_index;
        }
        return MTFS_OK;
    }
    for (i = 0U; i < runtime->region_count; ++i) {
        mtfs_error_t status = mtfs_sentinel_bundle_runtime_region_get(bundle,
            runtime_index, i, &region);
        if (status != MTFS_OK) return status;
        if (region.kind == MTFS_SENTINEL_REGION_EXECUTABLE_COPY) {
            if (++copy != 1 || region.placement != MTFS_SENTINEL_PLACEMENT_CALLER_RELATIVE ||
                region.storage_size != actual->copy_size ||
                region.alignment != actual->copy_alignment) return MTFS_ERROR_UNSUPPORTED_FORMAT;
        } else if (region.kind == MTFS_SENTINEL_REGION_ACTIVATION) {
            if (++activation != 1 || region.placement != MTFS_SENTINEL_PLACEMENT_FIXED_ABSOLUTE ||
                region.address_or_offset != actual->activation_address ||
                region.storage_size != actual->activation_size) return MTFS_ERROR_UNSUPPORTED_FORMAT;
        } else if (region.kind == MTFS_SENTINEL_REGION_PARAMETERS) {
            if (++parameters != 1 || region.placement != MTFS_SENTINEL_PLACEMENT_BINARY_CONTAINED ||
                region.address_or_offset != actual->parameters_offset ||
                region.logical_size != actual->parameters_logical_size ||
                region.storage_size != actual->parameters_storage_size)
                return MTFS_ERROR_UNSUPPORTED_FORMAT;
        } else if (region.kind == MTFS_SENTINEL_REGION_EXTERNAL_RW) {
            ++external;
            if (region.storage_size != actual->external_ram_size)
                return MTFS_ERROR_UNSUPPORTED_FORMAT;
        }
    }
    if (copy != 1 || activation != 1 || parameters != 1 ||
        (actual->external_ram_size == 0U ? external != 0 : external != 1))
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_npu_open(mtfs_sentinel_npu_context_t *context,
    const mtfs_sentinel_npu_provider_config_t *config,
    const mtfs_sentinel_bundle_t *bundle, uint32_t runtime_index,
    void *copy_memory, uint32_t copy_size,
    const mtfs_sentinel_runtime_region_policy_t *policies,
    uint32_t policy_count, uint32_t timeout_ms)
{
    mtfs_sentinel_npu_context_t opened;
    mtfs_sentinel_npu_actual_info_t actual;
    mtfs_sentinel_runtime_region_info_t region;
    mtfs_error_t status;
    uint32_t i;
    if (context == NULL || config == NULL || bundle == NULL || copy_memory == NULL ||
        config->api_version != MTFS_SENTINEL_NPU_PROVIDER_API_VERSION ||
        config->struct_size != sizeof(*config) || config->provider_id == 0U ||
        config->accelerator_id == 0U || config->ops == NULL ||
        config->ops->inspect == NULL || config->ops->install == NULL ||
        config->ops->infer == NULL || config->ops->close == NULL ||
        config->ops->lock == NULL || config->ops->unlock == NULL ||
        config->ops->zeroize == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    (void)memset(&opened, 0, sizeof(opened));
    opened.api_version = MTFS_SENTINEL_NPU_PROVIDER_API_VERSION;
    opened.struct_size = (uint16_t)sizeof(opened);
    opened.config = *config;
    opened.threshold_q8 = bundle->threshold_q8;
    status = mtfs_sentinel_bundle_runtime_get(bundle, runtime_index, &opened.runtime);
    if (status != MTFS_OK) return status;
    if (opened.runtime.provider_id != config->provider_id ||
        opened.runtime.accelerator_id != config->accelerator_id ||
        opened.runtime.runtime_type != MTFS_SENTINEL_RUNTIME_NPU)
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    status = mtfs_sentinel_bundle_runtime_regions_validate_policy(bundle, runtime_index,
        policies, policy_count, &opened.policy);
    if (status != MTFS_OK) return status;
    for (i = 0U; i < opened.runtime.region_count; ++i) {
        status = mtfs_sentinel_bundle_runtime_region_get(bundle, runtime_index, i, &region);
        if (status != MTFS_OK) return status;
        if (region.kind == MTFS_SENTINEL_REGION_EXECUTABLE_COPY) {
            if (region.storage_size > copy_size ||
                ((uintptr_t)copy_memory & (region.alignment - 1U)) != 0U)
                return MTFS_ERROR_BUFFER_TOO_SMALL;
            opened.copy_size = (uint32_t)region.storage_size;
        } else if (region.placement == MTFS_SENTINEL_PLACEMENT_FIXED_ABSOLUTE &&
            (opened.policy.zeroize_mask & (UINT32_C(1) << i)) != 0U) {
            if (region.address_or_offset > UINTPTR_MAX || region.storage_size > UINT32_MAX)
                return MTFS_ERROR_OVERFLOW;
            if (opened.fixed_zeroize_count >=
                MTFS_SENTINEL_NPU_MAX_FIXED_ZEROIZE_REGIONS)
                return MTFS_ERROR_NOT_SUPPORTED;
            opened.fixed_zeroize_memory[opened.fixed_zeroize_count] =
                (void *)(uintptr_t)region.address_or_offset;
            opened.fixed_zeroize_size[opened.fixed_zeroize_count] =
                (uint32_t)region.storage_size;
            ++opened.fixed_zeroize_count;
        }
    }
    (void)memset(&actual, 0, sizeof(actual));
    status = config->ops->inspect(config->target, opened.runtime.binary,
        opened.runtime.binary_size, &actual);
    if (status != MTFS_OK) return status;
    status = compare_actual(bundle, runtime_index, &opened.runtime, &actual);
    if (status != MTFS_OK) return status;
    status = config->ops->lock(config->target, timeout_ms);
    if (status != MTFS_OK) return status;
    opened.locked = 1U;
    status = config->ops->install(config->target, opened.runtime.binary,
        opened.runtime.binary_size, copy_memory, opened.copy_size,
        &opened.runtime);
    if (status != MTFS_OK) {
        config->ops->zeroize(config->target, copy_memory, opened.copy_size);
        for (i = 0U; i < opened.fixed_zeroize_count; ++i)
            config->ops->zeroize(config->target,
                opened.fixed_zeroize_memory[i], opened.fixed_zeroize_size[i]);
        config->ops->unlock(config->target);
        return status;
    }
    opened.copy_memory = copy_memory;
    opened.open = 1U;
    *context = opened;
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_npu_infer(mtfs_sentinel_npu_context_t *context,
    const int8_t input_q4[24], int8_t output_q4[24], uint32_t timeout_ms,
    mtfs_sentinel_inference_result_t *result)
{
    int8_t discarded_raw[24];
    return mtfs_sentinel_npu_infer_detailed(context, input_q4, discarded_raw,
                                             output_q4, timeout_ms, result);
}

mtfs_error_t mtfs_sentinel_npu_infer_detailed(
    mtfs_sentinel_npu_context_t *context, const int8_t input_q4[24],
    int8_t raw_output_int8[24], int8_t output_q4[24], uint32_t timeout_ms,
    mtfs_sentinel_inference_result_t *result)
{
    int8_t vendor_input[24], vendor_output[24], converted[24];
    uint32_t i;
    mtfs_error_t status;
    if (context == NULL || input_q4 == NULL || raw_output_int8 == NULL ||
        output_q4 == NULL || result == NULL ||
        context->api_version != MTFS_SENTINEL_NPU_PROVIDER_API_VERSION ||
        context->struct_size != sizeof(*context) || !context->open || !context->locked)
        return MTFS_ERROR_INVALID_STATE;
    for (i = 0U; i < 24U; ++i) {
        status = mtfs_sentinel_requantize_q4_to_int8(input_q4[i],
            context->runtime.input_scale_numerator,
            context->runtime.input_scale_shift, context->runtime.input_zero_point,
            &vendor_input[i]);
        if (status != MTFS_OK) goto cleanup;
    }
    status = context->config.ops->infer(context->config.target, vendor_input,
        vendor_output, timeout_ms);
    if (status != MTFS_OK) goto cleanup;
    for (i = 0U; i < 24U; ++i) {
        status = mtfs_sentinel_requantize_int8_to_q4(vendor_output[i],
            context->runtime.output_scale_numerator,
            context->runtime.output_scale_shift, context->runtime.output_zero_point,
            &converted[i]);
        if (status != MTFS_OK) goto cleanup;
    }
    status = mtfs_sentinel_score_q8(input_q4, converted,
        context->threshold_q8, result);
    if (status == MTFS_OK) {
        (void)memcpy(raw_output_int8, vendor_output, sizeof(vendor_output));
        (void)memcpy(output_q4, converted, sizeof(converted));
    }
cleanup:
    (void)memset(vendor_input, 0, sizeof(vendor_input));
    (void)memset(vendor_output, 0, sizeof(vendor_output));
    (void)memset(converted, 0, sizeof(converted));
    return status;
}

mtfs_error_t mtfs_sentinel_npu_close(mtfs_sentinel_npu_context_t *context,
    uint32_t timeout_ms)
{
    mtfs_error_t status;
    (void)timeout_ms;
    if (context == NULL || context->api_version != MTFS_SENTINEL_NPU_PROVIDER_API_VERSION ||
        context->struct_size != sizeof(*context) || !context->open || !context->locked)
        return MTFS_ERROR_INVALID_STATE;
    status = context->config.ops->close(context->config.target);
    if (status != MTFS_OK) return status;
    context->config.ops->zeroize(context->config.target, context->copy_memory,
        context->copy_size);
    {
        uint32_t i;
        for (i = 0U; i < context->fixed_zeroize_count; ++i)
        context->config.ops->zeroize(context->config.target,
            context->fixed_zeroize_memory[i], context->fixed_zeroize_size[i]);
    }
    context->config.ops->unlock(context->config.target);
    (void)memset(context, 0, sizeof(*context));
    return MTFS_OK;
}

#else
typedef int mtfs_sentinel_npu_provider_disabled_translation_unit_t;
#endif
