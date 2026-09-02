#include "mtfs_sentinel_inference.h"

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE

#include <limits.h>
#include <string.h>

#define COMPATIBILITY_SIZE (128U)
#define NORMALIZATION_SIZE (304U)
#define DECISION_SIZE (32U)
#define BUNDLE_MODEL_FORMAT (UINT32_C(0x534e5431))
#define BUNDLE_ACCELERATOR_CPU (UINT32_C(0x43505520))
#define SCHEMA_ID_SIZE (33U)

static const uint8_t bundle_magic[8] = {'M','T','F','S','S','B','1',0};
static const uint8_t cpu_magic[8] = {'M','T','F','S','Q','A','E','1'};
static const uint8_t schema_id[] = "mtfs-storage-sentinel-model-input";
static const uint8_t schema_hash[32] = {
    0x01,0xb0,0x04,0x05,0x33,0x49,0x1d,0x3f,
    0x2d,0x07,0x24,0x8b,0x34,0x59,0x09,0x44,
    0x7b,0xe0,0x04,0xd4,0xfc,0xd8,0xfc,0x94,
    0x71,0x9e,0xc2,0x8d,0x95,0x50,0x94,0xd8
};

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8U));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
        ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4U) << 32U);
}

static int32_t le_i32(const uint8_t *p)
{
    uint32_t value = le32(p);
    if (value <= (uint32_t)INT32_MAX) return (int32_t)value;
    return (int32_t)(-INT32_C(1) - (int32_t)(UINT32_MAX - value));
}

static int64_t le_i64(const uint8_t *p)
{
    uint64_t value = le64(p);
    if (value <= (uint64_t)INT64_MAX) return (int64_t)value;
    return -INT64_C(1) - (int64_t)(UINT64_MAX - value);
}

static int all_zero(const uint8_t *p, size_t size)
{
    size_t i;
    for (i = 0U; i < size; ++i) if (p[i] != 0U) return 0;
    return 1;
}

static int power_of_two(uint32_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static int known_section(uint16_t type)
{
    return type == MTFS_SENTINEL_SECTION_COMPATIBILITY ||
        type == MTFS_SENTINEL_SECTION_NORMALIZATION ||
        type == MTFS_SENTINEL_SECTION_DECISION ||
        type == MTFS_SENTINEL_SECTION_PROVENANCE ||
        type == MTFS_SENTINEL_SECTION_CPU_INT8_RUNTIME ||
        type == MTFS_SENTINEL_SECTION_NPU_RUNTIME;
}

static mtfs_error_t validate_cpu_model(const uint8_t *runtime, uint32_t size)
{
    const uint8_t *model;
    uint32_t binary_size, binary_offset;
    uint32_t i;
    static const uint16_t dimensions[5] = {24U,12U,4U,12U,24U};
    if (size < MTFS_SENTINEL_RUNTIME_DESCRIPTOR_SIZE)
        return MTFS_ERROR_MALFORMED_FORMAT;
    if (le16(runtime) != 1U || le16(runtime + 2U) != MTFS_SENTINEL_RUNTIME_CPU_INT8 ||
        le32(runtime + 4U) != MTFS_SENTINEL_PROVIDER_CPU_REFERENCE ||
        le32(runtime + 8U) != BUNDLE_ACCELERATOR_CPU ||
        le32(runtime + 12U) != MTFS_SENTINEL_MODEL_FORMAT_CPU_INT8_V1 ||
        le32(runtime + 16U) != 1U || le32(runtime + 20U) != 1U ||
        le16(runtime + 24U) != 24U || le16(runtime + 26U) != 24U ||
        runtime[28] != 1U || runtime[29] != 1U ||
        runtime[30] != 0U || runtime[31] != 0U ||
        le32(runtime + 32U) != 1U || le32(runtime + 36U) != 4U ||
        le32(runtime + 40U) != 1U || le32(runtime + 44U) != 4U ||
        le32(runtime + 48U) != 4U ||
        le32(runtime + 52U) != MTFS_SENTINEL_CPU_PERSISTENT_SIZE_32 ||
        le32(runtime + 56U) != MTFS_SENTINEL_CPU_WORK_SIZE)
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    binary_size = le32(runtime + 60U);
    binary_offset = le32(runtime + 160U);
    if (binary_size != MTFS_SENTINEL_CPU_MODEL_BINARY_SIZE ||
        binary_offset != MTFS_SENTINEL_RUNTIME_DESCRIPTOR_SIZE ||
        binary_size != size - binary_offset ||
        !all_zero(runtime + 164U, 28U))
        return MTFS_ERROR_MALFORMED_FORMAT;
    model = runtime + binary_offset;
    if (memcmp(model, cpu_magic, sizeof(cpu_magic)) != 0 ||
        le16(model + 8U) != 1U || le16(model + 10U) != 4U ||
        model[22] != 7U || model[23] != 4U || model[24] != 11U ||
        model[25] != 1U || !all_zero(model + 26U, 6U))
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    for (i = 0U; i < 5U; ++i)
        if (le16(model + 12U + i * 2U) != dimensions[i])
            return MTFS_ERROR_UNSUPPORTED_FORMAT;
    for (i = 0U; i < MTFS_SENTINEL_CPU_BIAS_COUNT; ++i) {
        int32_t bias = le_i32(model + 32U + MTFS_SENTINEL_CPU_WEIGHT_COUNT + i * 4U);
        if (bias > INT32_MAX - INT32_C(400000) ||
            bias < INT32_MIN + INT32_C(400000)) return MTFS_ERROR_OVERFLOW;
    }
    return MTFS_OK;
}

static mtfs_error_t validate_runtime(const uint8_t *section, uint32_t size,
    uint16_t expected_type, uint32_t directory_provider,
    uint32_t directory_alignment)
{
    uint16_t runtime_type;
    uint32_t binary_size, binary_offset, required_alignment;
    if (size < MTFS_SENTINEL_RUNTIME_DESCRIPTOR_SIZE)
        return MTFS_ERROR_MALFORMED_FORMAT;
    runtime_type = le16(section + 2U);
    binary_size = le32(section + 60U);
    binary_offset = le32(section + 160U);
    required_alignment = le32(section + 48U);
    if (le16(section) != 1U || le32(section + 4U) == 0U ||
        le32(section + 4U) != directory_provider ||
        le32(section + 8U) == 0U || le32(section + 12U) == 0U ||
        le32(section + 16U) == 0U || le32(section + 20U) != 1U ||
        le16(section + 24U) != 24U || le16(section + 26U) != 24U ||
        section[28] != 1U || section[29] != 1U ||
        section[30] != 0U || section[31] != 0U ||
        le32(section + 32U) == 0U || le32(section + 36U) > 31U ||
        le32(section + 40U) == 0U || le32(section + 44U) > 31U ||
        !power_of_two(required_alignment) || required_alignment > 4096U ||
        directory_alignment != required_alignment ||
        binary_offset < MTFS_SENTINEL_RUNTIME_DESCRIPTOR_SIZE ||
        (binary_offset & (required_alignment - 1U)) != 0U ||
        binary_offset > size || binary_size == 0U ||
        binary_size != size - binary_offset ||
        !all_zero(section + MTFS_SENTINEL_RUNTIME_DESCRIPTOR_SIZE,
            binary_offset - MTFS_SENTINEL_RUNTIME_DESCRIPTOR_SIZE) ||
        !all_zero(section + 164U, 28U)) return MTFS_ERROR_MALFORMED_FORMAT;
    if (expected_type == MTFS_SENTINEL_SECTION_CPU_INT8_RUNTIME) {
        if (runtime_type != MTFS_SENTINEL_RUNTIME_CPU_INT8)
            return MTFS_ERROR_UNSUPPORTED_FORMAT;
        return validate_cpu_model(section, size);
    }
    if (runtime_type != MTFS_SENTINEL_RUNTIME_NPU)
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_bundle_parse(const void *input, size_t input_size,
    const mtfs_sentinel_bundle_policy_t *policy,
    mtfs_sentinel_bundle_t *bundle)
{
    const uint8_t *bytes = (const uint8_t *)input;
    mtfs_sentinel_bundle_t parsed;
    uint32_t total, header_size, expected_offset, maximum;
    uint16_t count, i;
    unsigned compatibility_count = 0U, normalization_count = 0U;
    unsigned decision_count = 0U, provenance_count = 0U, cpu_count = 0U;
    unsigned npu_count = 0U;
    uint32_t runtime_providers[MTFS_SENTINEL_BUNDLE_MAX_RUNTIMES] = {0U, 0U};
    const uint8_t *canonical_model_hash = NULL;
    uint32_t npu_accelerator = 0U;
    if (bytes == NULL || policy == NULL || bundle == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (policy->api_version != MTFS_SENTINEL_INFERENCE_API_VERSION ||
        policy->struct_size != sizeof(*policy) ||
        policy->expected_target_id == 0U || policy->expected_transport_id == 0U ||
        policy->expected_accelerator_id == 0U ||
        policy->expected_model_format != BUNDLE_MODEL_FORMAT)
        return MTFS_ERROR_INVALID_ARGUMENT;
    maximum = policy->maximum_bundle_size == 0U ?
        MTFS_SENTINEL_BUNDLE_MAX_SIZE : policy->maximum_bundle_size;
    if (input_size < MTFS_SENTINEL_BUNDLE_HEADER_SIZE ||
        input_size > maximum || input_size > MTFS_SENTINEL_BUNDLE_MAX_SIZE ||
        input_size > UINT32_MAX) return MTFS_ERROR_MALFORMED_FORMAT;
    if (memcmp(bytes, bundle_magic, sizeof(bundle_magic)) != 0)
        return MTFS_ERROR_MALFORMED_FORMAT;
    if (le16(bytes + 8U) != MTFS_SENTINEL_BUNDLE_VERSION)
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    header_size = le16(bytes + 10U);
    total = le32(bytes + 12U);
    count = le16(bytes + 16U);
    if (count == 0U || count > MTFS_SENTINEL_BUNDLE_MAX_SECTIONS ||
        le16(bytes + 18U) != MTFS_SENTINEL_BUNDLE_DIRECTORY_ENTRY_SIZE ||
        le32(bytes + 20U) != 0U || !all_zero(bytes + 24U, 8U) ||
        header_size != MTFS_SENTINEL_BUNDLE_HEADER_SIZE +
            (uint32_t)count * MTFS_SENTINEL_BUNDLE_DIRECTORY_ENTRY_SIZE ||
        total != input_size || header_size > total)
        return MTFS_ERROR_MALFORMED_FORMAT;
    (void)memset(&parsed, 0, sizeof(parsed));
    parsed.api_version = MTFS_SENTINEL_INFERENCE_API_VERSION;
    parsed.struct_size = (uint16_t)sizeof(parsed);
    parsed.bytes = bytes;
    parsed.size = total;
    expected_offset = header_size;
    for (i = 0U; i < count; ++i) {
        const uint8_t *entry = bytes + MTFS_SENTINEL_BUNDLE_HEADER_SIZE +
            (uint32_t)i * MTFS_SENTINEL_BUNDLE_DIRECTORY_ENTRY_SIZE;
        uint16_t type = le16(entry), flags = le16(entry + 2U);
        uint32_t offset = le32(entry + 4U), length = le32(entry + 8U);
        uint32_t alignment = le32(entry + 12U), provider = le32(entry + 16U);
        uint32_t aligned;
        mtfs_error_t status;
        if ((flags & ~MTFS_SENTINEL_SECTION_FLAG_REQUIRED) != 0U ||
            !power_of_two(alignment) || alignment > 4096U || length == 0U ||
            !all_zero(entry + 20U, 12U)) return MTFS_ERROR_MALFORMED_FORMAT;
        if (expected_offset > UINT32_MAX - (alignment - 1U))
            return MTFS_ERROR_OVERFLOW;
        aligned = (expected_offset + alignment - 1U) & ~(alignment - 1U);
        if (offset != aligned || offset < header_size || offset > total ||
            length > total - offset || !all_zero(bytes + expected_offset,
                offset - expected_offset)) return MTFS_ERROR_MALFORMED_FORMAT;
        expected_offset = offset + length;
        if (!known_section(type)) {
            if ((flags & MTFS_SENTINEL_SECTION_FLAG_REQUIRED) != 0U)
                return MTFS_ERROR_UNSUPPORTED_FORMAT;
            continue;
        }
        if ((type == MTFS_SENTINEL_SECTION_COMPATIBILITY ||
             type == MTFS_SENTINEL_SECTION_NORMALIZATION ||
             type == MTFS_SENTINEL_SECTION_DECISION ||
             type == MTFS_SENTINEL_SECTION_PROVENANCE) &&
            (flags & MTFS_SENTINEL_SECTION_FLAG_REQUIRED) == 0U)
            return MTFS_ERROR_MALFORMED_FORMAT;
        if (type == MTFS_SENTINEL_SECTION_COMPATIBILITY) {
            ++compatibility_count;
            if (length != COMPATIBILITY_SIZE || le16(bytes + offset) != 1U ||
                le16(bytes + offset + 2U) != 1U ||
                le16(bytes + offset + 4U) != SCHEMA_ID_SIZE ||
                le16(bytes + offset + 6U) != 0U ||
                memcmp(bytes + offset + 8U, schema_hash, sizeof(schema_hash)) != 0 ||
                le32(bytes + offset + 48U) != MTFS_SENTINEL_TOPOLOGY_24_12_4_12_24 ||
                le32(bytes + offset + 56U) != 1U ||
                le32(bytes + offset + 64U) != BUNDLE_MODEL_FORMAT ||
                memcmp(bytes + offset + 72U, schema_id, SCHEMA_ID_SIZE) != 0 ||
                !all_zero(bytes + offset + 105U, 23U))
                return MTFS_ERROR_UNSUPPORTED_FORMAT;
            parsed.target_id = le32(bytes + offset + 40U);
            parsed.transport_id = le32(bytes + offset + 44U);
            parsed.profile_id = le32(bytes + offset + 52U);
            parsed.accelerator_id = le32(bytes + offset + 60U);
            parsed.model_format = le32(bytes + offset + 64U);
        } else if (type == MTFS_SENTINEL_SECTION_NORMALIZATION) {
            uint32_t n;
            ++normalization_count;
            if (length != NORMALIZATION_SIZE || le16(bytes + offset) != 1U ||
                le16(bytes + offset + 2U) != 24U ||
                le16(bytes + offset + 4U) != 16U ||
                le16(bytes + offset + 6U) != 20U ||
                le16(bytes + offset + 8U) != 4U ||
                le16(bytes + offset + 10U) != 1U ||
                le16(bytes + offset + 12U) != UINT16_C(0xff80) ||
                le16(bytes + offset + 14U) != 127U)
                return MTFS_ERROR_UNSUPPORTED_FORMAT;
            for (n = 0U; n < 24U; ++n) {
                parsed.normalization.mean_q16[n] =
                    le_i64(bytes + offset + 16U + n * 8U);
                parsed.normalization.inverse_std_q20[n] =
                    le_i32(bytes + offset + 208U + n * 4U);
                if (parsed.normalization.inverse_std_q20[n] <= 0)
                    return MTFS_ERROR_MALFORMED_FORMAT;
            }
        } else if (type == MTFS_SENTINEL_SECTION_DECISION) {
            ++decision_count;
            if (length != DECISION_SIZE || le16(bytes + offset) != 1U ||
                le16(bytes + offset + 2U) != 24U ||
                le16(bytes + offset + 4U) != 4U ||
                le16(bytes + offset + 6U) != 4U ||
                le16(bytes + offset + 8U) != 8U ||
                le16(bytes + offset + 10U) != 1U ||
                le16(bytes + offset + 12U) != 1U ||
                le16(bytes + offset + 14U) != 0U ||
                le64(bytes + offset + 24U) != UINT64_C(65025) ||
                le64(bytes + offset + 16U) > le64(bytes + offset + 24U))
                return MTFS_ERROR_MALFORMED_FORMAT;
            parsed.threshold_q8 = le64(bytes + offset + 16U);
        } else if (type == MTFS_SENTINEL_SECTION_PROVENANCE) {
            ++provenance_count;
            parsed.provenance = bytes + offset;
            parsed.provenance_size = length;
        } else {
            uint32_t runtime_index;
            ++parsed.runtime_count;
            if (parsed.runtime_count > MTFS_SENTINEL_BUNDLE_MAX_RUNTIMES)
                return MTFS_ERROR_UNSUPPORTED_FORMAT;
            if (provider == 0U) return MTFS_ERROR_MALFORMED_FORMAT;
            for (runtime_index = 0U;
                 runtime_index + 1U < parsed.runtime_count; ++runtime_index)
                if (runtime_providers[runtime_index] == provider)
                    return MTFS_ERROR_MALFORMED_FORMAT;
            runtime_providers[parsed.runtime_count - 1U] = provider;
            status = validate_runtime(bytes + offset, length, type, provider,
                alignment);
            if (status != MTFS_OK) return status;
            if (canonical_model_hash == NULL)
                canonical_model_hash = bytes + offset + 64U;
            else if (memcmp(canonical_model_hash, bytes + offset + 64U, 32U) != 0)
                return MTFS_ERROR_UNSUPPORTED_FORMAT;
            if (type == MTFS_SENTINEL_SECTION_CPU_INT8_RUNTIME) {
                ++cpu_count;
                parsed.cpu_runtime = bytes + offset;
                parsed.cpu_runtime_size = length;
            } else {
                uint32_t runtime_accelerator = le32(bytes + offset + 8U);
                ++npu_count;
                if (npu_count > 1U) return MTFS_ERROR_UNSUPPORTED_FORMAT;
                npu_accelerator = runtime_accelerator;
            }
        }
    }
    if (expected_offset != total || compatibility_count != 1U ||
        normalization_count != 1U || decision_count != 1U ||
        provenance_count != 1U || cpu_count > 1U || parsed.runtime_count == 0U)
        return MTFS_ERROR_MALFORMED_FORMAT;
    if ((npu_count == 0U &&
         parsed.accelerator_id != MTFS_SENTINEL_OUTER_ACCELERATOR_CPU) ||
        (npu_count == 1U && parsed.accelerator_id != npu_accelerator))
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    if (parsed.target_id != policy->expected_target_id ||
        parsed.transport_id != policy->expected_transport_id ||
        parsed.accelerator_id != policy->expected_accelerator_id ||
        parsed.model_format != policy->expected_model_format ||
        (policy->expected_profile_id != 0U &&
         parsed.profile_id != policy->expected_profile_id))
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    if (!((parsed.target_id == MTFS_SENTINEL_TARGET_EK_RA8P1 &&
           parsed.transport_id == MTFS_SENTINEL_TRANSPORT_SPI) ||
          (parsed.target_id == MTFS_SENTINEL_TARGET_STM32N6570_DK &&
           parsed.transport_id == MTFS_SENTINEL_TRANSPORT_SDMMC_IDMA)))
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    {
        mtfs_sentinel_memory_plan_t plan;
        mtfs_error_t status = mtfs_sentinel_bundle_memory_plan(&parsed, &plan);
        if (status != MTFS_OK) return status;
    }
    *bundle = parsed;
    return MTFS_OK;
}

typedef struct runtime_section_view
{
    const uint8_t *section;
    uint32_t size;
    uint32_t alignment;
    uint32_t provider;
    uint16_t section_type;
} runtime_section_view_t;

static mtfs_error_t runtime_section_at(const mtfs_sentinel_bundle_t *bundle,
    uint32_t requested_index, runtime_section_view_t *view)
{
    uint16_t count, i;
    uint32_t runtime_index = 0U;
    if (bundle == NULL || view == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (bundle->api_version != MTFS_SENTINEL_INFERENCE_API_VERSION ||
        bundle->struct_size != sizeof(*bundle) || bundle->bytes == NULL ||
        bundle->size < MTFS_SENTINEL_BUNDLE_HEADER_SIZE ||
        bundle->runtime_count == 0U ||
        bundle->runtime_count > MTFS_SENTINEL_BUNDLE_MAX_RUNTIMES)
        return MTFS_ERROR_INVALID_STATE;
    if (requested_index >= bundle->runtime_count)
        return MTFS_ERROR_OUT_OF_RANGE;
    count = le16(bundle->bytes + 16U);
    if (count == 0U || count > MTFS_SENTINEL_BUNDLE_MAX_SECTIONS)
        return MTFS_ERROR_INVALID_STATE;
    if (le16(bundle->bytes + 10U) != MTFS_SENTINEL_BUNDLE_HEADER_SIZE +
            (uint32_t)count * MTFS_SENTINEL_BUNDLE_DIRECTORY_ENTRY_SIZE ||
        le16(bundle->bytes + 10U) > bundle->size ||
        le32(bundle->bytes + 12U) != bundle->size)
        return MTFS_ERROR_INVALID_STATE;
    for (i = 0U; i < count; ++i) {
        const uint8_t *entry = bundle->bytes + MTFS_SENTINEL_BUNDLE_HEADER_SIZE +
            (uint32_t)i * MTFS_SENTINEL_BUNDLE_DIRECTORY_ENTRY_SIZE;
        uint16_t type = le16(entry);
        if (type == MTFS_SENTINEL_SECTION_CPU_INT8_RUNTIME ||
            type == MTFS_SENTINEL_SECTION_NPU_RUNTIME) {
            if (runtime_index == requested_index) {
                uint32_t offset = le32(entry + 4U);
                uint32_t length = le32(entry + 8U);
                if (offset > bundle->size || length > bundle->size - offset)
                    return MTFS_ERROR_INVALID_STATE;
                view->section = bundle->bytes + offset;
                view->size = length;
                view->alignment = le32(entry + 12U);
                view->provider = le32(entry + 16U);
                view->section_type = type;
                return MTFS_OK;
            }
            ++runtime_index;
        }
    }
    return MTFS_ERROR_INVALID_STATE;
}

static mtfs_error_t decode_runtime_info(const runtime_section_view_t *view,
    mtfs_sentinel_runtime_info_t *runtime)
{
    mtfs_sentinel_runtime_info_t decoded;
    uint32_t binary_offset;
    mtfs_error_t status = validate_runtime(view->section, view->size,
        view->section_type, view->provider, view->alignment);
    if (status != MTFS_OK) return status;
    binary_offset = le32(view->section + 160U);
    (void)memset(&decoded, 0, sizeof(decoded));
    decoded.api_version = MTFS_SENTINEL_INFERENCE_API_VERSION;
    decoded.struct_size = (uint16_t)sizeof(decoded);
    decoded.runtime_type = le16(view->section + 2U);
    decoded.provider_id = le32(view->section + 4U);
    decoded.accelerator_id = le32(view->section + 8U);
    decoded.model_format = le32(view->section + 12U);
    decoded.model_version = le32(view->section + 16U);
    decoded.runtime_abi = le32(view->section + 20U);
    decoded.input_dimension = le16(view->section + 24U);
    decoded.output_dimension = le16(view->section + 26U);
    decoded.input_data_type = view->section[28U];
    decoded.output_data_type = view->section[29U];
    decoded.input_zero_point = (int8_t)view->section[30U];
    decoded.output_zero_point = (int8_t)view->section[31U];
    decoded.input_scale_numerator = le32(view->section + 32U);
    decoded.input_scale_shift = le32(view->section + 36U);
    decoded.output_scale_numerator = le32(view->section + 40U);
    decoded.output_scale_shift = le32(view->section + 44U);
    decoded.required_alignment = le32(view->section + 48U);
    decoded.persistent_memory = le32(view->section + 52U);
    decoded.scratch_memory = le32(view->section + 56U);
    decoded.binary_size = le32(view->section + 60U);
    decoded.binary = view->section + binary_offset;
    (void)memcpy(decoded.canonical_model_hash, view->section + 64U, 32U);
    (void)memcpy(decoded.runtime_binary_hash, view->section + 96U, 32U);
    (void)memcpy(decoded.conversion_manifest_hash, view->section + 128U, 32U);
    *runtime = decoded;
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_bundle_runtime_count(
    const mtfs_sentinel_bundle_t *bundle, uint32_t *runtime_count)
{
    runtime_section_view_t view;
    if (runtime_count == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    if (runtime_section_at(bundle, 0U, &view) != MTFS_OK)
        return MTFS_ERROR_INVALID_STATE;
    *runtime_count = bundle->runtime_count;
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_bundle_runtime_get(
    const mtfs_sentinel_bundle_t *bundle, uint32_t index,
    mtfs_sentinel_runtime_info_t *runtime)
{
    runtime_section_view_t view;
    mtfs_error_t status;
    if (runtime == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    status = runtime_section_at(bundle, index, &view);
    if (status != MTFS_OK) return status;
    return decode_runtime_info(&view, runtime);
}

mtfs_error_t mtfs_sentinel_bundle_runtime_find(
    const mtfs_sentinel_bundle_t *bundle, uint32_t provider_id,
    uint32_t accelerator_id, mtfs_sentinel_runtime_info_t *runtime)
{
    mtfs_sentinel_runtime_info_t candidate, found;
    uint32_t count, i, matches = 0U;
    mtfs_error_t status;
    if (provider_id == 0U || accelerator_id == 0U || runtime == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    (void)memset(&found, 0, sizeof(found));
    status = mtfs_sentinel_bundle_runtime_count(bundle, &count);
    if (status != MTFS_OK) return status;
    for (i = 0U; i < count; ++i) {
        status = mtfs_sentinel_bundle_runtime_get(bundle, i, &candidate);
        if (status != MTFS_OK) return status;
        if (candidate.provider_id == provider_id &&
            candidate.accelerator_id == accelerator_id) {
            found = candidate;
            ++matches;
        }
    }
    if (matches == 0U) return MTFS_ERROR_NOT_FOUND;
    if (matches != 1U) return MTFS_ERROR_MALFORMED_FORMAT;
    *runtime = found;
    return MTFS_OK;
}

static mtfs_error_t align_ram_cursor(uint64_t cursor, uint32_t alignment,
    uint64_t *aligned)
{
    uint64_t mask = (uint64_t)alignment - 1U;
    if (cursor > MTFS_SENTINEL_MAX_REQUIRED_RAM - mask)
        return MTFS_ERROR_OVERFLOW;
    *aligned = (cursor + mask) & ~mask;
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_bundle_memory_plan(
    const mtfs_sentinel_bundle_t *bundle, mtfs_sentinel_memory_plan_t *plan)
{
    mtfs_sentinel_memory_plan_t calculated;
    mtfs_sentinel_runtime_info_t runtime;
    uint64_t cursor;
    uint32_t count, i;
    mtfs_error_t status;
    if (plan == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    status = mtfs_sentinel_bundle_runtime_count(bundle, &count);
    if (status != MTFS_OK) return status;
    (void)memset(&calculated, 0, sizeof(calculated));
    calculated.api_version = MTFS_SENTINEL_INFERENCE_API_VERSION;
    calculated.struct_size = (uint16_t)sizeof(calculated);
    calculated.runtime_count = count;
    calculated.required_alignment = 1U;
    calculated.bundle_size = bundle->size;
    cursor = bundle->size;
    for (i = 0U; i < count; ++i) {
        status = mtfs_sentinel_bundle_runtime_get(bundle, i, &runtime);
        if (status != MTFS_OK) return status;
        if (runtime.required_alignment > calculated.required_alignment)
            calculated.required_alignment = runtime.required_alignment;
        if (runtime.scratch_memory > calculated.scratch_size)
            calculated.scratch_size = runtime.scratch_memory;
        calculated.persistent_size[i] = runtime.persistent_memory;
        if (runtime.persistent_memory != 0U) {
            status = align_ram_cursor(cursor, runtime.required_alignment, &cursor);
            if (status != MTFS_OK ||
                runtime.persistent_memory > MTFS_SENTINEL_MAX_REQUIRED_RAM - cursor)
                return MTFS_ERROR_OVERFLOW;
            calculated.persistent_offset[i] = cursor;
            cursor += runtime.persistent_memory;
        }
    }
    if (calculated.scratch_size != 0U) {
        status = align_ram_cursor(cursor, calculated.required_alignment, &cursor);
        if (status != MTFS_OK ||
            calculated.scratch_size > MTFS_SENTINEL_MAX_REQUIRED_RAM - cursor)
            return MTFS_ERROR_OVERFLOW;
        calculated.scratch_offset = cursor;
        cursor += calculated.scratch_size;
    }
    calculated.required_ram = cursor;
    *plan = calculated;
    return MTFS_OK;
}

/* Exact rounded ratio in ten fixed steps; no uint64 division is emitted. */
static uint32_t permille(uint64_t numerator, uint64_t denominator)
{
    uint64_t remainder = 0U;
    uint32_t quotient = 0U, bit;
    if (denominator == 0U || numerator > denominator) return UINT32_MAX;
    for (bit = 10U; bit != 0U; --bit) {
        uint32_t carry = 0U;
        quotient <<= 1U;
        if (remainder >= denominator - remainder) {
            remainder -= denominator - remainder;
            carry = 1U;
        } else remainder += remainder;
        if ((UINT32_C(1000) & (UINT32_C(1) << (bit - 1U))) != 0U) {
            if (numerator >= denominator - remainder) {
                remainder = numerator - (denominator - remainder);
                ++carry;
            } else remainder += numerator;
        }
        quotient += carry;
    }
    if (remainder != 0U && remainder >= denominator - remainder) ++quotient;
    return quotient > 1000U ? 1000U : quotient;
}

static int add_u64(uint64_t *value, uint64_t addend)
{
    if (addend > UINT64_MAX - *value) return 0;
    *value += addend;
    return 1;
}

/* 32-bit limbs avoid both overflow and a target uint64 multiply helper. */
static int multiply_u64(uint64_t a, uint64_t b, uint64_t *product)
{
    uint64_t a_low = (uint32_t)a, a_high = a >> 32U;
    uint64_t b_low = (uint32_t)b, b_high = b >> 32U;
    uint64_t low_product, cross_a, cross_b, high;
    if (a_high != 0U && b_high != 0U) return 0;
    low_product = a_low * b_low;
    cross_a = a_high * b_low;
    cross_b = a_low * b_high;
    if (cross_a > UINT32_MAX || cross_b > UINT32_MAX) return 0;
    high = (low_product >> 32U) + cross_a + cross_b;
    if (high > UINT32_MAX) return 0;
    *product = (high << 32U) | (uint32_t)low_product;
    return 1;
}

static int encoder_timing_consistent(
    const mtfs_sentinel_operation_feature_t *operation)
{
    uint64_t total = 0U, average_total;
    uint32_t bucket;
    if (operation->timing_samples == 0U) return 0;
    for (bucket = 0U; bucket < MTFS_SENTINEL_HISTOGRAM_BUCKETS; ++bucket)
        if (!add_u64(&total, operation->latency_histogram[bucket])) return 0;
    if (total != operation->timing_samples ||
        !multiply_u64(operation->average_latency_us,
            operation->timing_samples, &average_total) ||
        average_total > operation->total_latency_us)
        return 0;
    return operation->total_latency_us - average_total <
        operation->timing_samples;
}

static int hard_fault(const mtfs_sentinel_feature_v1_t *f)
{
    uint32_t mask = f->transport.validity_mask;
    if (f->io_errors != 0U || f->not_ready_errors != 0U ||
        f->no_media_errors != 0U || f->timeout_errors != 0U) return 1;
    return ((mask & MTFS_SENTINEL_TRANSPORT_VALID_TRANSPORT_ERRORS) != 0U &&
            f->transport.transport_errors != 0U) ||
        ((mask & MTFS_SENTINEL_TRANSPORT_VALID_TRANSFER_TIMEOUTS) != 0U &&
            f->transport.transfer_timeouts != 0U) ||
        ((mask & MTFS_SENTINEL_TRANSPORT_VALID_READY_TIMEOUTS) != 0U &&
            f->transport.ready_timeouts != 0U) ||
        ((mask & MTFS_SENTINEL_TRANSPORT_VALID_ABORTS) != 0U &&
            f->transport.aborts != 0U) ||
        ((mask & MTFS_SENTINEL_TRANSPORT_VALID_CLOCK_ERRORS) != 0U &&
            f->transport.clock_errors != 0U);
}

mtfs_error_t mtfs_sentinel_feature_encode_raw(
    const mtfs_sentinel_feature_v1_t *feature, uint32_t output[24])
{
    uint32_t temporary[24];
    uint64_t timings[3], total_timing = 0U;
    uint32_t op, out = 0U;
    static const uint8_t first[5] = {0U,4U,8U,12U,15U};
    static const uint8_t last[5] = {3U,7U,11U,14U,21U};
    if (feature == NULL || output == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    if (feature->version != MTFS_SENTINEL_SCHEMA_VERSION ||
        feature->struct_size != sizeof(*feature) || feature->sample_count == 0U)
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    if ((feature->validity_mask & MTFS_SENTINEL_VALID_REQUIRED) !=
            MTFS_SENTINEL_VALID_REQUIRED ||
        (feature->flags & (MTFS_SENTINEL_FLAG_NO_ACTIVITY |
            MTFS_SENTINEL_FLAG_INSUFFICIENT_DATA |
            MTFS_SENTINEL_FLAG_DISCONTINUITY |
            MTFS_SENTINEL_FLAG_COUNTER_SATURATED |
            MTFS_SENTINEL_FLAG_TIMING_UNAVAILABLE)) != 0U || hard_fault(feature))
        return MTFS_ERROR_NOT_READY;
    for (op = 0U; op < 3U; ++op) {
        const mtfs_sentinel_operation_feature_t *operation = &feature->operation[op];
        uint64_t histogram_total = 0U, denominator;
        uint32_t group, bucket;
        if (operation->timing_samples == 0U ||
            !encoder_timing_consistent(operation) ||
            !add_u64(&total_timing, operation->timing_samples))
            return MTFS_ERROR_MALFORMED_FORMAT;
        timings[op] = operation->timing_samples;
        temporary[out++] = operation->average_latency_us > UINT64_C(1000000) ?
            UINT32_C(1000000) : (uint32_t)operation->average_latency_us;
        denominator = operation->timing_samples;
        if (!add_u64(&denominator, operation->timing_invalid))
            return MTFS_ERROR_OVERFLOW;
        temporary[out++] = permille(operation->timing_invalid, denominator);
        for (group = 0U; group < 5U; ++group) {
            uint64_t group_total = 0U;
            for (bucket = first[group]; bucket <= last[group]; ++bucket)
                if (!add_u64(&group_total, operation->latency_histogram[bucket]) ||
                    !add_u64(&histogram_total,
                        operation->latency_histogram[bucket]))
                    return MTFS_ERROR_OVERFLOW;
            temporary[out++] = permille(group_total, operation->timing_samples);
        }
        if (histogram_total != operation->timing_samples)
            return MTFS_ERROR_MALFORMED_FORMAT;
    }
    if (total_timing == 0U) return MTFS_ERROR_NOT_READY;
    for (op = 0U; op < 3U; ++op) temporary[out++] = permille(timings[op], total_timing);
    if (out != 24U) return MTFS_ERROR_INVALID_STATE;
    (void)memcpy(output, temporary, sizeof(temporary));
    return MTFS_OK;
}

static mtfs_error_t rounded_shift_q36_to_q4(int64_t value, int32_t *result)
{
    uint64_t magnitude, rounded;
    const uint64_t half = UINT64_C(1) << 31U;
    if (value >= 0) {
        magnitude = (uint64_t)value;
        rounded = (magnitude + half) >> 32U;
        *result = rounded > INT32_MAX ? INT32_MAX : (int32_t)rounded;
    } else {
        magnitude = (uint64_t)(-(value + 1)) + 1U;
        rounded = (magnitude + half) >> 32U;
        if (rounded > (uint64_t)INT32_MAX + 1U) *result = INT32_MIN;
        else if (rounded == (uint64_t)INT32_MAX + 1U) *result = INT32_MIN;
        else *result = -(int32_t)rounded;
    }
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_normalize_int8(
    const mtfs_sentinel_normalization_t *normalization,
    const uint32_t raw[24], int8_t output[24])
{
    int8_t temporary[24];
    uint32_t i;
    if (normalization == NULL || raw == NULL || output == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    for (i = 0U; i < 24U; ++i) {
        int64_t raw_q16, delta, product;
        uint64_t magnitude, product_magnitude;
        int32_t quantized;
        int32_t inverse = normalization->inverse_std_q20[i];
        if (inverse <= 0)
            return MTFS_ERROR_MALFORMED_FORMAT;
        raw_q16 = (int64_t)((uint64_t)raw[i] << 16U);
        if ((normalization->mean_q16[i] < 0 &&
             raw_q16 > INT64_MAX + normalization->mean_q16[i]) ||
            (normalization->mean_q16[i] > 0 &&
             raw_q16 < INT64_MIN + normalization->mean_q16[i]))
            return MTFS_ERROR_OVERFLOW;
        delta = raw_q16 - normalization->mean_q16[i];
        magnitude = delta >= 0 ? (uint64_t)delta :
            (uint64_t)(-(delta + 1)) + 1U;
        if (!multiply_u64(magnitude, (uint32_t)inverse,
                &product_magnitude) ||
            (delta >= 0 && product_magnitude > INT64_MAX) ||
            (delta < 0 && product_magnitude > (uint64_t)INT64_MAX + 1U))
            return MTFS_ERROR_OVERFLOW;
        if (delta >= 0) product = (int64_t)product_magnitude;
        else if (product_magnitude == (uint64_t)INT64_MAX + 1U)
            product = INT64_MIN;
        else product = -(int64_t)product_magnitude;
        (void)rounded_shift_q36_to_q4(product, &quantized);
        if (quantized < -128) quantized = -128;
        if (quantized > 127) quantized = 127;
        temporary[i] = (int8_t)quantized;
    }
    (void)memcpy(output, temporary, sizeof(temporary));
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_cpu_init(mtfs_sentinel_cpu_context_t *context,
    const mtfs_sentinel_bundle_t *bundle)
{
    mtfs_sentinel_cpu_context_t initialized;
    const uint8_t *model;
    if (context == NULL || bundle == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    if (bundle->api_version != MTFS_SENTINEL_INFERENCE_API_VERSION ||
        bundle->struct_size != sizeof(*bundle) || bundle->cpu_runtime == NULL ||
        validate_cpu_model(bundle->cpu_runtime, bundle->cpu_runtime_size) != MTFS_OK)
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    model = bundle->cpu_runtime + le32(bundle->cpu_runtime + 160U);
    (void)memset(&initialized, 0, sizeof(initialized));
    initialized.api_version = MTFS_SENTINEL_INFERENCE_API_VERSION;
    initialized.struct_size = (uint16_t)sizeof(initialized);
    initialized.weights = model + MTFS_SENTINEL_CPU_MODEL_BINARY_HEADER_SIZE;
    initialized.biases = initialized.weights + MTFS_SENTINEL_CPU_WEIGHT_COUNT;
    initialized.weights_size = MTFS_SENTINEL_CPU_WEIGHT_COUNT;
    initialized.biases_size = MTFS_SENTINEL_CPU_BIAS_COUNT * 4U;
    initialized.threshold_q8 = bundle->threshold_q8;
    *context = initialized;
    return MTFS_OK;
}

static int ranges_overlap(const void *a, size_t as, const void *b, size_t bs)
{
    uintptr_t ap = (uintptr_t)a, bp = (uintptr_t)b;
    if (as == 0U || bs == 0U) return 0;
    if (ap > UINTPTR_MAX - as || bp > UINTPTR_MAX - bs) return 1;
    return ap < bp + bs && bp < ap + as;
}

static int32_t round_shift7(int64_t value)
{
    if (value >= 0) return (int32_t)((value + 64) / 128);
    return -(int32_t)(((uint64_t)(-(value + 1)) + 1U + 64U) / 128U);
}

mtfs_error_t mtfs_sentinel_cpu_infer(
    const mtfs_sentinel_cpu_context_t *context, const int8_t input[24],
    void *work, size_t work_size, int8_t output[24],
    mtfs_sentinel_inference_result_t *result)
{
    static const uint8_t dimensions[5] = {24U,12U,4U,12U,24U};
    uint8_t *storage = (uint8_t *)work;
    int8_t *current, *next;
    uint32_t layer, weight_offset = 0U, bias_offset = 0U;
    uint32_t squared_sum = 0U;
    mtfs_sentinel_inference_result_t completed;
    if (context == NULL || input == NULL || work == NULL || output == NULL ||
        result == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    if (context->api_version != MTFS_SENTINEL_INFERENCE_API_VERSION ||
        context->struct_size != sizeof(*context) || context->weights == NULL ||
        context->biases == NULL || context->weights_size != 672U ||
        context->biases_size != 208U || work_size < MTFS_SENTINEL_CPU_WORK_SIZE)
        return MTFS_ERROR_INVALID_STATE;
    if (ranges_overlap(input, 24U, output, 24U) ||
        ranges_overlap(input, 24U, work, MTFS_SENTINEL_CPU_WORK_SIZE) ||
        ranges_overlap(input, 24U, result, sizeof(*result)) ||
        ranges_overlap(output, 24U, work, MTFS_SENTINEL_CPU_WORK_SIZE) ||
        ranges_overlap(result, sizeof(*result), output, 24U) ||
        ranges_overlap(result, sizeof(*result), work, MTFS_SENTINEL_CPU_WORK_SIZE))
        return MTFS_ERROR_INVALID_ARGUMENT;
    current = (int8_t *)storage;
    next = (int8_t *)(storage + 24U);
    (void)memcpy(current, input, 24U);
    for (layer = 0U; layer < 4U; ++layer) {
        uint32_t in_count = dimensions[layer], out_count = dimensions[layer + 1U];
        uint32_t o, n;
        for (o = 0U; o < out_count; ++o) {
            int64_t accumulator = le_i32(context->biases + bias_offset + o * 4U);
            int32_t quantized;
            for (n = 0U; n < in_count; ++n) {
                int8_t weight = (int8_t)context->weights[
                    weight_offset + n * out_count + o];
                accumulator += (int32_t)current[n] * (int32_t)weight;
            }
            if (accumulator < INT32_MIN || accumulator > INT32_MAX)
                return MTFS_ERROR_OVERFLOW;
            quantized = round_shift7(accumulator);
            if (layer != 3U && quantized < 0) quantized = 0;
            if (quantized < -128) quantized = -128;
            if (quantized > 127) quantized = 127;
            next[o] = (int8_t)quantized;
        }
        weight_offset += in_count * out_count;
        bias_offset += out_count * 4U;
        { int8_t *swap = current; current = next; next = swap; }
    }
    if (weight_offset != 672U || bias_offset != 208U)
        return MTFS_ERROR_INVALID_STATE;
    for (layer = 0U; layer < 24U; ++layer) {
        int32_t difference = (int32_t)current[layer] - (int32_t)input[layer];
        uint32_t square = (uint32_t)(difference * difference);
        if (square > UINT32_MAX - squared_sum) return MTFS_ERROR_OVERFLOW;
        squared_sum += square;
    }
    (void)memset(&completed, 0, sizeof(completed));
    completed.api_version = MTFS_SENTINEL_INFERENCE_API_VERSION;
    completed.struct_size = (uint16_t)sizeof(completed);
    completed.score_q8 = (squared_sum + 12U) / 24U;
    completed.threshold_q8 = context->threshold_q8;
    completed.anomaly = completed.score_q8 > completed.threshold_q8 ? 1U : 0U;
    (void)memcpy(output, current, 24U);
    *result = completed;
    return MTFS_OK;
}

#else
typedef int mtfs_sentinel_inference_disabled_translation_unit_t;
#endif /* MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE */
