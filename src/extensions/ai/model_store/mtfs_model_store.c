#include "mtfs_model_store.h"

#if MTFS_ENABLE_SEALED_MODEL

#include <limits.h>
#include <string.h>

static int mtfs_model_ranges_overlap(const void *a, size_t a_size,
                                     const void *b, size_t b_size)
{
    uintptr_t a_start;
    uintptr_t b_start;
    if (a_size == 0U || b_size == 0U)
        return 0;
    if (a == NULL || b == NULL)
        return 1;
    a_start = (uintptr_t)a;
    b_start = (uintptr_t)b;
    if (a_size > UINTPTR_MAX - a_start || b_size > UINTPTR_MAX - b_start)
        return 1;
    return a_start < b_start + b_size && b_start < a_start + a_size;
}

static int mtfs_model_overlaps_work(const mtfs_model_t *model,
                                    const void *pointer, size_t size)
{
    const mtfs_sealed_work_t *work = &model->sealed_blob.work;
    return mtfs_model_ranges_overlap(pointer, size, work->manifest,
                                     work->manifest_capacity) ||
           mtfs_model_ranges_overlap(pointer, size, work->aad,
                                     work->aad_capacity) ||
           mtfs_model_ranges_overlap(pointer, size, work->ciphertext,
                                     work->ciphertext_capacity) ||
           mtfs_model_ranges_overlap(pointer, size, work->plaintext,
                                     work->plaintext_capacity);
}

static mtfs_error_t mtfs_model_validate_policy(const mtfs_model_policy_t *policy)
{
    if (policy == NULL ||
        policy->api_version != MTFS_MODEL_POLICY_API_VERSION ||
        policy->struct_size < sizeof(*policy))
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (policy->expected_target_id == MTFS_MODEL_ID_INVALID ||
        policy->expected_accelerator_id == MTFS_MODEL_ID_INVALID ||
        policy->accepted_model_format == MTFS_MODEL_ID_INVALID ||
        policy->maximum_chunk_size == 0U)
        return MTFS_ERROR_INVALID_ARGUMENT;
    return MTFS_OK;
}

static mtfs_error_t mtfs_model_apply_policy(
    const mtfs_sealed_package_info_t *sealed,
    const mtfs_model_policy_t *policy)
{
    if (sealed->object_type != MTFS_SEALED_OBJECT_TYPE_AI_MODEL ||
        sealed->model_format != policy->accepted_model_format)
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    if (sealed->target_id != policy->expected_target_id ||
        sealed->accelerator_id != policy->expected_accelerator_id ||
        sealed->chunk_plain_size > policy->maximum_chunk_size ||
        sealed->payload_plain_length > policy->maximum_model_payload_size ||
        sealed->required_ram > policy->maximum_required_ram)
        return MTFS_ERROR_NOT_SUPPORTED;
    if (sealed->required_ram < sealed->payload_plain_length)
        return MTFS_ERROR_MALFORMED_FORMAT;
    return MTFS_OK;
}

static void mtfs_model_copy_info(mtfs_model_info_t *destination,
                                 const mtfs_sealed_package_info_t *source)
{
    memset(destination, 0, sizeof(*destination));
    destination->api_version = MTFS_MODEL_INFO_API_VERSION;
    destination->struct_size = (uint32_t)sizeof(*destination);
    memcpy(destination->model_id, source->model_id,
           sizeof(destination->model_id));
    destination->model_version = source->model_version;
    destination->target_id = source->target_id;
    destination->accelerator_id = source->accelerator_id;
    destination->model_format = source->model_format;
    destination->payload_size = source->payload_plain_length;
    destination->required_ram = source->required_ram;
    destination->chunk_size = source->chunk_plain_size;
    destination->chunk_count = source->chunk_count;
}

void mtfs_model_init(mtfs_model_t *model)
{
    if (model != NULL)
    {
        memset(model, 0, sizeof(*model));
        model->api_version = MTFS_MODEL_API_VERSION;
        model->struct_size = (uint32_t)sizeof(*model);
        model->state = MTFS_MODEL_CLOSED;
        mtfs_sealed_blob_init(&model->sealed_blob);
    }
}

mtfs_error_t mtfs_model_open(mtfs_model_t *model,
    const mtfs_sealed_reader_t *reader, const mtfs_crypto_provider_t *provider,
    const mtfs_sealed_work_t *work, const mtfs_model_policy_t *policy)
{
    mtfs_sealed_package_info_t sealed_info;
    mtfs_error_t result;
    if (model == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (model->api_version != MTFS_MODEL_API_VERSION ||
        model->struct_size < sizeof(*model) || model->state != MTFS_MODEL_CLOSED)
        return MTFS_ERROR_INVALID_STATE;
    result = mtfs_model_validate_policy(policy);
    if (result != MTFS_OK)
        return result;
    model->state = MTFS_MODEL_OPENING;
    result = mtfs_sealed_blob_open(&model->sealed_blob, reader, provider, work,
                                   &sealed_info);
    if (result != MTFS_OK)
    {
        model->state = MTFS_MODEL_ERROR;
        return result;
    }
    result = mtfs_model_apply_policy(&sealed_info, policy);
    if (result != MTFS_OK)
    {
        (void)mtfs_sealed_blob_close(&model->sealed_blob);
        memset(&model->info, 0, sizeof(model->info));
        model->state = MTFS_MODEL_ERROR;
        return result;
    }
    mtfs_model_copy_info(&model->info, &sealed_info);
    model->state = MTFS_MODEL_OPEN;
    return MTFS_OK;
}

mtfs_error_t mtfs_model_get_info(const mtfs_model_t *model,
                                 mtfs_model_info_t *info)
{
    if (model == NULL || info == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (model->api_version != MTFS_MODEL_API_VERSION ||
        model->struct_size < sizeof(*model) ||
        (model->state != MTFS_MODEL_OPEN && model->state != MTFS_MODEL_LOADED))
        return MTFS_ERROR_INVALID_STATE;
    if (mtfs_model_ranges_overlap(info, sizeof(*info), model, sizeof(*model)) ||
        mtfs_model_overlaps_work(model, info, sizeof(*info)))
        return MTFS_ERROR_INVALID_ARGUMENT;
    *info = model->info;
    return MTFS_OK;
}

mtfs_error_t mtfs_model_load(mtfs_model_t *model, void *destination,
    size_t destination_size, size_t *loaded_size)
{
    mtfs_error_t result;
    if (model == NULL || loaded_size == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (model->api_version != MTFS_MODEL_API_VERSION ||
        model->struct_size < sizeof(*model) || model->state != MTFS_MODEL_OPEN)
    {
        *loaded_size = 0U;
        return MTFS_ERROR_INVALID_STATE;
    }
    if (mtfs_model_ranges_overlap(loaded_size, sizeof(*loaded_size),
                                  destination, destination_size) ||
        mtfs_model_ranges_overlap(loaded_size, sizeof(*loaded_size),
                                  model, sizeof(*model)) ||
        mtfs_model_overlaps_work(model, loaded_size, sizeof(*loaded_size)) ||
        mtfs_model_ranges_overlap(destination, destination_size,
                                  model, sizeof(*model)))
        return MTFS_ERROR_INVALID_ARGUMENT;
    *loaded_size = 0U;
    if (model->info.payload_size > SIZE_MAX || model->info.required_ram > SIZE_MAX ||
        destination_size < (size_t)model->info.payload_size ||
        destination_size < (size_t)model->info.required_ram ||
        (destination == NULL && (model->info.payload_size != 0U ||
                                 model->info.required_ram != 0U)))
        return MTFS_ERROR_BUFFER_TOO_SMALL;
    model->state = MTFS_MODEL_LOADING;
    result = mtfs_sealed_blob_load(&model->sealed_blob, destination,
                                   destination_size, loaded_size);
    if (result == MTFS_OK)
        model->state = MTFS_MODEL_LOADED;
    else if (model->sealed_blob.state == MTFS_SEALED_BLOB_OPEN)
        model->state = MTFS_MODEL_OPEN;
    else
    {
        memset(&model->info, 0, sizeof(model->info));
        model->state = MTFS_MODEL_ERROR;
    }
    return result;
}

mtfs_error_t mtfs_model_close(mtfs_model_t *model)
{
    mtfs_error_t result;
    if (model == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (model->api_version != MTFS_MODEL_API_VERSION ||
        model->struct_size < sizeof(*model))
        return MTFS_ERROR_INVALID_STATE;
    result = mtfs_sealed_blob_close(&model->sealed_blob);
    mtfs_model_init(model);
    return result;
}

#else
typedef int mtfs_model_store_disabled_translation_unit_t;
#endif /* MTFS_ENABLE_SEALED_MODEL */
