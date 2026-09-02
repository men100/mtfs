#include "mtfs_sentinel_sealed_adapter.h"

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE && MTFS_ENABLE_SEALED_MODEL

mtfs_error_t mtfs_sentinel_bundle_parse_model_payload(
    const void *payload, size_t payload_size,
    const mtfs_model_info_t *outer_model,
    uint32_t expected_transport_id, uint32_t expected_profile_id,
    mtfs_sentinel_bundle_t *bundle)
{
    mtfs_sentinel_bundle_policy_t policy;
    mtfs_sentinel_bundle_t parsed;
    mtfs_sentinel_memory_plan_t plan;
    mtfs_error_t status;
    if (payload == NULL || outer_model == NULL || bundle == NULL ||
        outer_model->api_version != MTFS_MODEL_INFO_API_VERSION ||
        outer_model->struct_size != sizeof(*outer_model) ||
        outer_model->target_id == 0U || outer_model->accelerator_id == 0U ||
        expected_transport_id == 0U ||
        outer_model->model_format != MTFS_MODEL_FORMAT_SENTINEL_BUNDLE_V1 ||
        outer_model->payload_size != payload_size)
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (outer_model->required_ram > MTFS_SENTINEL_MAX_REQUIRED_RAM)
        return MTFS_ERROR_OVERFLOW;
    policy.api_version = MTFS_SENTINEL_INFERENCE_API_VERSION;
    policy.struct_size = (uint16_t)sizeof(policy);
    policy.expected_target_id = outer_model->target_id;
    policy.expected_transport_id = expected_transport_id;
    policy.expected_accelerator_id = outer_model->accelerator_id;
    policy.expected_model_format = outer_model->model_format;
    policy.expected_profile_id = expected_profile_id;
    policy.maximum_bundle_size = MTFS_SENTINEL_BUNDLE_MAX_SIZE;
    status = mtfs_sentinel_bundle_parse(payload, payload_size, &policy, &parsed);
    if (status != MTFS_OK) return status;
    status = mtfs_sentinel_bundle_memory_plan(&parsed, &plan);
    if (status != MTFS_OK) return status;
    if (outer_model->required_ram < plan.required_ram)
        return MTFS_ERROR_BUFFER_TOO_SMALL;
    if (((uintptr_t)payload & (plan.required_alignment - 1U)) != 0U)
        return MTFS_ERROR_INVALID_ARGUMENT;
    *bundle = parsed;
    return MTFS_OK;
}

#else
typedef int mtfs_sentinel_sealed_adapter_disabled_translation_unit_t;
#endif
