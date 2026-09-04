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

static int known_region_kind(uint16_t kind)
{
    return kind >= MTFS_SENTINEL_REGION_EXECUTABLE_COPY &&
        kind <= MTFS_SENTINEL_REGION_PROVIDER_CONTEXT;
}

static int known_region_placement(uint16_t placement)
{
    return placement >= MTFS_SENTINEL_PLACEMENT_CALLER_RELATIVE &&
        placement <= MTFS_SENTINEL_PLACEMENT_PROVIDER_ASSIGNED;
}

static mtfs_error_t validate_npu_regions(const uint8_t *section,
    uint32_t binary_offset, uint32_t binary_size)
{
    uint32_t table_offset = le32(section + 164U);
    uint16_t count = le16(section + 168U);
    uint16_t entry_size = le16(section + 170U);
    uint64_t caller_end = 0U;
    uint32_t caller_alignment = 1U;
    uint16_t i, j;
    const uint32_t access_mask = MTFS_SENTINEL_REGION_ACCESS_READ |
        MTFS_SENTINEL_REGION_ACCESS_WRITE | MTFS_SENTINEL_REGION_ACCESS_EXECUTE;
    const uint32_t requirement_mask = MTFS_SENTINEL_REGION_REQUIRE_ZEROIZE |
        MTFS_SENTINEL_REGION_REQUIRE_CACHE_COHERENCY |
        MTFS_SENTINEL_REGION_REQUIRE_EXCLUSIVE |
        MTFS_SENTINEL_REGION_REQUIRE_SHAREABLE |
        MTFS_SENTINEL_REGION_REQUIRE_INPUT_OUTPUT_SHARED;
    if (table_offset != MTFS_SENTINEL_RUNTIME_DESCRIPTOR_SIZE || count == 0U ||
        count > MTFS_SENTINEL_RUNTIME_REGION_MAX_COUNT ||
        entry_size != MTFS_SENTINEL_RUNTIME_REGION_ENTRY_SIZE ||
        le16(section + 172U) != MTFS_SENTINEL_RUNTIME_DESCRIPTOR_SIZE ||
        le16(section + 174U) != 0U || !all_zero(section + 188U, 4U) ||
        (uint32_t)count > (binary_offset - table_offset) / entry_size)
        return MTFS_ERROR_MALFORMED_FORMAT;
    for (i = 0U; i < count; ++i) {
        const uint8_t *entry = section + table_offset + (uint32_t)i * entry_size;
        uint16_t kind = le16(entry + 2U), placement = le16(entry + 4U);
        uint64_t logical = le64(entry + 8U), address = le64(entry + 24U);
        uint64_t storage = le64(entry + 48U);
        uint32_t alignment = le32(entry + 16U);
        uint32_t lifetime = le32(entry + 32U);
        if (le16(entry) != 1U || !known_region_kind(kind) ||
            !known_region_placement(placement) ||
            le16(entry + 6U) != MTFS_SENTINEL_RUNTIME_REGION_FLAG_REQUIRED ||
            logical == 0U || storage < logical || storage > UINT64_MAX - address ||
            !power_of_two(alignment) ||
            alignment > 4096U || lifetime < MTFS_SENTINEL_REGION_LIFETIME_INSTALL ||
            lifetime > MTFS_SENTINEL_REGION_LIFETIME_INFERENCE ||
            (le32(entry + 36U) & ~access_mask) != 0U ||
            (le32(entry + 40U) & ~access_mask) != 0U ||
            (le32(entry + 44U) & ~requirement_mask) != 0U ||
            !all_zero(entry + 56U, 8U)) return MTFS_ERROR_MALFORMED_FORMAT;
        for (j = 0U; j < i; ++j) {
            const uint8_t *prior = section + table_offset + (uint32_t)j * entry_size;
            if (le16(prior + 2U) == kind && le16(prior + 4U) == placement &&
                le64(prior + 24U) == address)
                return MTFS_ERROR_MALFORMED_FORMAT;
            if (le16(prior + 4U) == placement &&
                placement != MTFS_SENTINEL_PLACEMENT_PROVIDER_ASSIGNED) {
                uint64_t prior_address = le64(prior + 24U);
                uint64_t prior_storage = le64(prior + 48U);
                if (address < prior_address + prior_storage &&
                    prior_address < address + storage)
                    return MTFS_ERROR_MALFORMED_FORMAT;
            }
        }
        if (placement == MTFS_SENTINEL_PLACEMENT_CALLER_RELATIVE) {
            if ((address & ((uint64_t)alignment - 1U)) != 0U ||
                storage > UINT32_MAX || address > UINT32_MAX - storage)
                return MTFS_ERROR_MALFORMED_FORMAT;
            if (address + storage > caller_end) caller_end = address + storage;
            if (alignment > caller_alignment) caller_alignment = alignment;
        } else if (placement == MTFS_SENTINEL_PLACEMENT_FIXED_ABSOLUTE) {
            if (address == 0U || (address & ((uint64_t)alignment - 1U)) != 0U ||
                storage > UINT64_MAX - address)
                return MTFS_ERROR_MALFORMED_FORMAT;
        } else if (placement == MTFS_SENTINEL_PLACEMENT_BINARY_CONTAINED) {
            if (address > binary_size || storage > binary_size - address)
                return MTFS_ERROR_MALFORMED_FORMAT;
        } else if (address != 0U) return MTFS_ERROR_MALFORMED_FORMAT;
    }
    if (!all_zero(section + table_offset + (uint32_t)count * entry_size,
            binary_offset - table_offset - (uint32_t)count * entry_size) ||
        caller_end != le32(section + 52U) || le32(section + 56U) != 0U ||
        caller_alignment != le32(section + 48U))
        return MTFS_ERROR_MALFORMED_FORMAT;
    return MTFS_OK;
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
    if (le16(runtime + 2U) != MTFS_SENTINEL_RUNTIME_CPU_INT8 ||
        le32(runtime + 4U) != MTFS_SENTINEL_PROVIDER_CPU_REFERENCE ||
        le32(runtime + 8U) != BUNDLE_ACCELERATOR_CPU ||
        le32(runtime + 20U) != 1U ||
        le16(runtime + 24U) != 24U || le16(runtime + 26U) != 24U ||
        runtime[28] != 1U || runtime[29] != 1U ||
        le32(runtime + 48U) != 4U ||
        le32(runtime + 52U) != MTFS_SENTINEL_CPU_PERSISTENT_SIZE_32 ||
        le32(runtime + 56U) != MTFS_SENTINEL_CPU_WORK_SIZE)
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    binary_size = le32(runtime + 60U);
    binary_offset = le32(runtime + 160U);
    if (binary_offset != MTFS_SENTINEL_RUNTIME_DESCRIPTOR_SIZE ||
        binary_size != size - binary_offset ||
        !all_zero(runtime + 164U, 28U))
        return MTFS_ERROR_MALFORMED_FORMAT;
    model = runtime + binary_offset;
    if (binary_size < 8U) return MTFS_ERROR_MALFORMED_FORMAT;
    if (le16(runtime) == 1U &&
        le32(runtime + 12U) == MTFS_SENTINEL_MODEL_FORMAT_CPU_INT8_V1 &&
        le32(runtime + 16U) == 1U) {
        if (runtime[30] != 0U || runtime[31] != 0U ||
            le32(runtime + 32U) != 1U || le32(runtime + 36U) != 4U ||
            le32(runtime + 40U) != 1U || le32(runtime + 44U) != 4U ||
            binary_size != MTFS_SENTINEL_CPU_MODEL_BINARY_SIZE)
            return MTFS_ERROR_UNSUPPORTED_FORMAT;
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
    if (le16(runtime) != 2U ||
        le32(runtime + 12U) != MTFS_SENTINEL_MODEL_FORMAT_CPU_TFLITE_INT8_V2 ||
        le32(runtime + 16U) != 2U ||
        binary_size < MTFS_SENTINEL_CPU_TFLITE_INT8_HEADER_SIZE ||
        binary_size > MTFS_SENTINEL_CPU_TFLITE_INT8_MAX_BINARY_SIZE ||
        runtime[30] != model[41U] ||
        runtime[31] != model[43U] || le32(runtime + 32U) != le32(model + 36U) ||
        le32(runtime + 36U) != model[40U] || le32(runtime + 40U) != le32(model + 44U) ||
        le32(runtime + 44U) != model[42U] ||
        memcmp(model, "MTFSTI82", 8U) != 0 || le16(model + 8U) != 2U ||
        le16(model + 10U) != 4U || model[22U] != 4U || model[23U] != 4U ||
        le32(model + 24U) != 2U || le32(model + 28U) != binary_size ||
        le32(model + 32U) != MTFS_SENTINEL_CPU_TFLITE_INT8_HEADER_SIZE ||
        le32(model + 36U) == 0U || model[40U] > 31U || model[42U] > 31U ||
        le32(model + 44U) == 0U ||
        memcmp(model + 48U, runtime + 64U, 32U) != 0 ||
        !all_zero(model + 80U, 16U))
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    for (i = 0U; i < 5U; ++i)
        if (le16(model + 12U + i * 2U) != dimensions[i])
            return MTFS_ERROR_UNSUPPORTED_FORMAT;
    for (i = 0U; i < 4U; ++i) {
        const uint8_t *layer = model + MTFS_SENTINEL_CPU_TFLITE_INT8_HEADER_SIZE +
            i * MTFS_SENTINEL_CPU_TFLITE_LAYER_DESCRIPTOR_SIZE;
        uint32_t out_count = dimensions[i + 1U], in_count = dimensions[i];
        uint32_t offsets[6], sizes[6], j, o, n;
        if (le16(layer) != in_count || le16(layer + 2U) != out_count ||
            layer[4U] != (i == 3U ? 0U : 1U) || layer[5U] != 0U ||
            !all_zero(layer + 40U, 8U)) return MTFS_ERROR_UNSUPPORTED_FORMAT;
        if ((le32(layer + 8U) & UINT32_C(0x80000000)) != 0U ||
            (le32(layer + 8U) & UINT32_C(0x7f800000)) == 0U ||
            (le32(layer + 8U) & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) ||
            (le32(layer + 12U) & UINT32_C(0x80000000)) != 0U ||
            (le32(layer + 12U) & UINT32_C(0x7f800000)) == 0U ||
            (le32(layer + 12U) & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000))
            return MTFS_ERROR_UNSUPPORTED_FORMAT;
        if (i != 0U) {
            const uint8_t *prior = layer - MTFS_SENTINEL_CPU_TFLITE_LAYER_DESCRIPTOR_SIZE;
            if (le32(prior + 12U) != le32(layer + 8U) || prior[7U] != layer[6U])
                return MTFS_ERROR_UNSUPPORTED_FORMAT;
        }
        sizes[0] = in_count * out_count; sizes[1] = out_count * 4U;
        sizes[2] = out_count * 4U; sizes[3] = out_count;
        sizes[4] = out_count * 4U; sizes[5] = out_count;
        for (j = 0U; j < 6U; ++j) {
            offsets[j] = le32(layer + 16U + j * 4U);
            if (offsets[j] < MTFS_SENTINEL_CPU_TFLITE_INT8_HEADER_SIZE +
                    4U * MTFS_SENTINEL_CPU_TFLITE_LAYER_DESCRIPTOR_SIZE ||
                offsets[j] > binary_size || sizes[j] > binary_size - offsets[j])
                return MTFS_ERROR_MALFORMED_FORMAT;
        }
        for (o = 0U; o < out_count; ++o) {
            int64_t bound = le_i32(model + offsets[1] + o * 4U);
            int32_t input_zero = (int8_t)layer[6U];
            int32_t weight_zero = (int8_t)model[offsets[3] + o];
            int32_t input_bound = 127 - input_zero;
            if (input_bound < input_zero + 128) input_bound = input_zero + 128;
            if (le_i32(model + offsets[4] + o * 4U) < 0 ||
                (int8_t)model[offsets[5] + o] < -31 ||
                (int8_t)model[offsets[5] + o] > 30)
                return MTFS_ERROR_UNSUPPORTED_FORMAT;
            bound = bound < 0 ? -bound : bound;
            for (n = 0U; n < in_count; ++n) {
                int32_t weight = (int8_t)model[offsets[0] + o * in_count + n] - weight_zero;
                if (weight < 0) weight = -weight;
                bound += (int64_t)input_bound * weight;
            }
            if (bound > INT32_MAX) return MTFS_ERROR_OVERFLOW;
        }
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
    if (le32(section + 4U) == 0U ||
        le32(section + 4U) != directory_provider ||
        le32(section + 8U) == 0U || le32(section + 12U) == 0U ||
        le32(section + 16U) == 0U || le32(section + 20U) == 0U ||
        le16(section + 24U) != 24U || le16(section + 26U) != 24U ||
        section[28] != 1U || section[29] != 1U ||
        le32(section + 32U) == 0U || le32(section + 36U) > 31U ||
        le32(section + 40U) == 0U || le32(section + 44U) > 31U ||
        !power_of_two(required_alignment) || required_alignment > 4096U ||
        directory_alignment != required_alignment ||
        binary_offset < MTFS_SENTINEL_RUNTIME_DESCRIPTOR_SIZE ||
        (binary_offset & (required_alignment - 1U)) != 0U ||
        binary_offset > size || binary_size == 0U ||
        binary_size != size - binary_offset) return MTFS_ERROR_MALFORMED_FORMAT;
    if (expected_type == MTFS_SENTINEL_SECTION_CPU_INT8_RUNTIME) {
        if ((le16(section) != MTFS_SENTINEL_RUNTIME_DESCRIPTOR_CPU_VERSION &&
             le16(section) != 2U) ||
            runtime_type != MTFS_SENTINEL_RUNTIME_CPU_INT8 ||
            !all_zero(section + MTFS_SENTINEL_RUNTIME_DESCRIPTOR_SIZE,
                binary_offset - MTFS_SENTINEL_RUNTIME_DESCRIPTOR_SIZE) ||
            !all_zero(section + 164U, 28U))
            return MTFS_ERROR_UNSUPPORTED_FORMAT;
        return validate_cpu_model(section, size);
    }
    if (le16(section) != MTFS_SENTINEL_RUNTIME_DESCRIPTOR_NPU_VERSION ||
        runtime_type != MTFS_SENTINEL_RUNTIME_NPU)
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    return validate_npu_regions(section, binary_offset, binary_size);
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
    uint16_t runtime_index;
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
                view->runtime_index = (uint16_t)runtime_index;
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
    decoded.runtime_index = view->runtime_index;
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
    decoded.descriptor_version = le16(view->section);
    if (decoded.descriptor_version == MTFS_SENTINEL_RUNTIME_DESCRIPTOR_NPU_VERSION) {
        decoded.region_count = le16(view->section + 168U);
        decoded.runtime_version_major = le16(view->section + 176U);
        decoded.runtime_version_minor = le16(view->section + 178U);
        decoded.runtime_variant = le32(view->section + 180U);
        decoded.runtime_extra = le32(view->section + 184U);
    }
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

mtfs_error_t mtfs_sentinel_bundle_runtime_region_get(
    const mtfs_sentinel_bundle_t *bundle, uint32_t runtime_index,
    uint32_t region_index, mtfs_sentinel_runtime_region_info_t *region)
{
    runtime_section_view_t view;
    mtfs_sentinel_runtime_region_info_t decoded;
    const uint8_t *entry;
    uint32_t binary_offset;
    mtfs_error_t status;
    if (region == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    status = runtime_section_at(bundle, runtime_index, &view);
    if (status != MTFS_OK) return status;
    status = validate_runtime(view.section, view.size, view.section_type,
        view.provider, view.alignment);
    if (status != MTFS_OK) return status;
    if (le16(view.section) != MTFS_SENTINEL_RUNTIME_DESCRIPTOR_NPU_VERSION ||
        region_index >= le16(view.section + 168U)) return MTFS_ERROR_NOT_FOUND;
    binary_offset = le32(view.section + 160U);
    if (binary_offset > view.size) return MTFS_ERROR_INVALID_STATE;
    entry = view.section + le32(view.section + 164U) +
        region_index * MTFS_SENTINEL_RUNTIME_REGION_ENTRY_SIZE;
    (void)memset(&decoded, 0, sizeof(decoded));
    decoded.api_version = MTFS_SENTINEL_INFERENCE_API_VERSION;
    decoded.struct_size = (uint16_t)sizeof(decoded);
    decoded.kind = le16(entry + 2U);
    decoded.placement = le16(entry + 4U);
    decoded.flags = le16(entry + 6U);
    decoded.region_index = region_index;
    decoded.logical_size = le64(entry + 8U);
    decoded.alignment = le32(entry + 16U);
    decoded.provider_pool_id = le32(entry + 20U);
    decoded.address_or_offset = le64(entry + 24U);
    decoded.lifetime = le32(entry + 32U);
    decoded.install_access = le32(entry + 36U);
    decoded.inference_access = le32(entry + 40U);
    decoded.requirements = le32(entry + 44U);
    decoded.storage_size = le64(entry + 48U);
    *region = decoded;
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_bundle_runtime_regions_validate_policy(
    const mtfs_sentinel_bundle_t *bundle, uint32_t runtime_index,
    const mtfs_sentinel_runtime_region_policy_t *policies,
    uint32_t policy_count, mtfs_sentinel_runtime_policy_result_t *result)
{
    mtfs_sentinel_runtime_info_t runtime;
    mtfs_sentinel_runtime_region_info_t region;
    mtfs_sentinel_runtime_policy_result_t checked;
    uint32_t i, j;
    mtfs_error_t status;
    if (policies == NULL || policy_count == 0U || result == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    status = mtfs_sentinel_bundle_runtime_get(bundle, runtime_index, &runtime);
    if (status != MTFS_OK) return status;
    if (runtime.descriptor_version != MTFS_SENTINEL_RUNTIME_DESCRIPTOR_NPU_VERSION ||
        runtime.region_count == 0U) return MTFS_ERROR_NOT_SUPPORTED;
    (void)memset(&checked, 0, sizeof(checked));
    checked.api_version = MTFS_SENTINEL_INFERENCE_API_VERSION;
    checked.struct_size = (uint16_t)sizeof(checked);
    checked.region_count = runtime.region_count;
    for (i = 0U; i < runtime.region_count; ++i) {
        const mtfs_sentinel_runtime_region_policy_t *match = NULL;
        uint32_t matches = 0U;
        status = mtfs_sentinel_bundle_runtime_region_get(bundle, runtime_index, i,
            &region);
        if (status != MTFS_OK) return status;
        for (j = 0U; j < policy_count; ++j) {
            const mtfs_sentinel_runtime_region_policy_t *candidate = &policies[j];
            if (candidate->provider_id == runtime.provider_id &&
                candidate->accelerator_id == runtime.accelerator_id &&
                candidate->kind == region.kind &&
                candidate->placement == region.placement &&
                (region.placement == MTFS_SENTINEL_PLACEMENT_PROVIDER_ASSIGNED ||
                 (candidate->address_limit > candidate->address_minimum &&
                  region.address_or_offset >= candidate->address_minimum &&
                  region.address_or_offset < candidate->address_limit &&
                  region.storage_size <= candidate->address_limit -
                      region.address_or_offset))) {
                match = candidate;
                ++matches;
            }
        }
        if (matches != 1U || match == NULL ||
            !power_of_two(match->minimum_alignment) ||
            region.alignment < match->minimum_alignment ||
            (region.alignment & (match->minimum_alignment - 1U)) != 0U ||
            region.storage_size > match->maximum_storage_size ||
            (region.install_access & ~match->allowed_install_access) != 0U ||
            (region.inference_access & ~match->allowed_inference_access) != 0U ||
            (region.requirements & ~match->allowed_requirements) != 0U)
            return MTFS_ERROR_NOT_SUPPORTED;
        if (region.placement == MTFS_SENTINEL_PLACEMENT_PROVIDER_ASSIGNED) {
            if (match->address_minimum != 0U || match->address_limit != 0U)
                return MTFS_ERROR_NOT_SUPPORTED;
        } else if (match->address_limit <= match->address_minimum ||
            region.address_or_offset < match->address_minimum ||
            region.address_or_offset >= match->address_limit ||
            region.storage_size > match->address_limit - region.address_or_offset)
            return MTFS_ERROR_NOT_SUPPORTED;
        if (region.placement == MTFS_SENTINEL_PLACEMENT_FIXED_ABSOLUTE &&
            (match->policy_flags & MTFS_SENTINEL_REGION_POLICY_OWNED) == 0U)
            return MTFS_ERROR_NOT_SUPPORTED;
        if ((region.requirements & MTFS_SENTINEL_REGION_REQUIRE_EXCLUSIVE) != 0U &&
            (match->policy_flags & MTFS_SENTINEL_REGION_POLICY_EXCLUSIVE) == 0U)
            return MTFS_ERROR_NOT_SUPPORTED;
        if ((region.requirements & MTFS_SENTINEL_REGION_REQUIRE_CACHE_COHERENCY) != 0U &&
            (match->policy_flags & MTFS_SENTINEL_REGION_POLICY_CACHE_MAINTENANCE) == 0U)
            return MTFS_ERROR_NOT_SUPPORTED;
        if ((region.requirements & MTFS_SENTINEL_REGION_REQUIRE_ZEROIZE) != 0U &&
            (match->policy_flags & (MTFS_SENTINEL_REGION_POLICY_OWNED |
             MTFS_SENTINEL_REGION_POLICY_ALLOW_ZEROIZE)) !=
            (MTFS_SENTINEL_REGION_POLICY_OWNED |
             MTFS_SENTINEL_REGION_POLICY_ALLOW_ZEROIZE))
            return MTFS_ERROR_NOT_SUPPORTED;
        checked.accepted_mask |= UINT32_C(1) << i;
        if ((match->policy_flags & MTFS_SENTINEL_REGION_POLICY_OWNED) != 0U)
            checked.owned_mask |= UINT32_C(1) << i;
        if ((match->policy_flags & MTFS_SENTINEL_REGION_POLICY_CACHE_MAINTENANCE) != 0U)
            checked.cache_maintenance_mask |= UINT32_C(1) << i;
        if ((match->policy_flags & MTFS_SENTINEL_REGION_POLICY_ALLOW_ZEROIZE) != 0U)
            checked.zeroize_mask |= UINT32_C(1) << i;
        if ((match->policy_flags & MTFS_SENTINEL_REGION_POLICY_GLOBAL_SERIALIZATION) != 0U)
            checked.global_serialization_required = 1U;
    }
    *result = checked;
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
        uint32_t slot;
        status = mtfs_sentinel_bundle_runtime_get(bundle, i, &runtime);
        if (status != MTFS_OK) return status;
        slot = runtime.runtime_index;
        if (slot != i || slot >= MTFS_SENTINEL_BUNDLE_MAX_RUNTIMES)
            return MTFS_ERROR_INVALID_STATE;
        if (runtime.required_alignment > calculated.required_alignment)
            calculated.required_alignment = runtime.required_alignment;
        if (runtime.scratch_memory > calculated.scratch_size)
            calculated.scratch_size = runtime.scratch_memory;
        calculated.persistent_size[slot] = runtime.persistent_memory;
        if (runtime.persistent_memory != 0U) {
            status = align_ram_cursor(cursor, runtime.required_alignment, &cursor);
            if (status != MTFS_OK ||
                runtime.persistent_memory > MTFS_SENTINEL_MAX_REQUIRED_RAM - cursor)
                return MTFS_ERROR_OVERFLOW;
            calculated.persistent_offset[slot] = cursor;
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
    if (context == NULL || bundle == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    if (bundle->api_version != MTFS_SENTINEL_INFERENCE_API_VERSION ||
        bundle->struct_size != sizeof(*bundle) || bundle->cpu_runtime == NULL ||
        validate_cpu_model(bundle->cpu_runtime, bundle->cpu_runtime_size) != MTFS_OK)
        return MTFS_ERROR_UNSUPPORTED_FORMAT;
    (void)memset(&initialized, 0, sizeof(initialized));
    initialized.api_version = MTFS_SENTINEL_INFERENCE_API_VERSION;
    initialized.struct_size = (uint16_t)sizeof(initialized);
    initialized.runtime = bundle->cpu_runtime;
    initialized.runtime_size = bundle->cpu_runtime_size;
    initialized.model_format = le32(bundle->cpu_runtime + 12U);
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

static int32_t tflite_high_mul(int32_t a, int32_t b)
{
    int64_t product, nudge;
    if (a == INT32_MIN && b == INT32_MIN) return INT32_MAX;
    product = (int64_t)a * b;
    nudge = product >= 0 ? INT64_C(1073741824) : -INT64_C(1073741823);
    product += nudge;
    if (product >= 0) return (int32_t)(product / INT64_C(2147483648));
    return -(int32_t)((-product) / INT64_C(2147483648));
}

static int32_t tflite_divide_pot(int32_t value, uint32_t exponent)
{
    int64_t denominator, base, remainder, threshold;
    if (exponent == 0U) return value;
    denominator = INT64_C(1) << exponent;
    if (value >= 0) base = value / denominator;
    else base = -(((int64_t)(-(value + 1)) + 1 + denominator - 1) / denominator);
    remainder = (int64_t)value - base * denominator;
    threshold = (denominator - 1) / 2 + (value < 0 ? 1 : 0);
    return (int32_t)(base + (remainder > threshold ? 1 : 0));
}

static mtfs_error_t tflite_multiply(int32_t value, int32_t multiplier,
    int8_t shift, int32_t *output)
{
    uint32_t left = shift > 0 ? (uint32_t)shift : 0U;
    uint32_t right = shift < 0 ? (uint32_t)(-shift) : 0U;
    int64_t shifted = (int64_t)value * (INT64_C(1) << left);
    if (multiplier < 0 || shift < -31 || shift > 30 ||
        shifted < INT32_MIN || shifted > INT32_MAX)
        return MTFS_ERROR_OVERFLOW;
    *output = tflite_divide_pot(tflite_high_mul((int32_t)shifted, multiplier), right);
    return MTFS_OK;
}

static int8_t q4_to_canonical(int8_t value, uint32_t numerator,
    uint32_t shift, int8_t zero_point)
{
    uint64_t magnitude = (uint64_t)(value < 0 ? -(int32_t)value : value) << shift;
    uint64_t denominator = (uint64_t)numerator * 16U;
    int64_t quantized = (int64_t)((magnitude + denominator / 2U) / denominator);
    if (value < 0) quantized = -quantized;
    quantized += zero_point;
    if (quantized < -128) quantized = -128;
    if (quantized > 127) quantized = 127;
    return (int8_t)quantized;
}

static int8_t canonical_to_q4(int8_t value, uint32_t numerator,
    uint32_t shift, int8_t zero_point)
{
    int64_t product = ((int64_t)value - zero_point) * numerator * 16U;
    uint64_t magnitude = product < 0 ? (uint64_t)(-product) : (uint64_t)product;
    int64_t quantized = shift == 0U ? (int64_t)magnitude :
        (int64_t)((magnitude + (UINT64_C(1) << (shift - 1U))) >> shift);
    if (product < 0) quantized = -quantized;
    if (quantized < -128) quantized = -128;
    if (quantized > 127) quantized = 127;
    return (int8_t)quantized;
}

mtfs_error_t mtfs_sentinel_cpu_infer_canonical_int8(
    const mtfs_sentinel_cpu_context_t *context, const int8_t input[24],
    void *work, size_t work_size, int8_t output[24])
{
    static const uint8_t dimensions[5] = {24U,12U,4U,12U,24U};
    const uint8_t *model;
    uint8_t *storage = (uint8_t *)work;
    int8_t *current, *next;
    uint32_t layer_index;
    if (context == NULL || input == NULL || work == NULL || output == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (context->api_version != MTFS_SENTINEL_INFERENCE_API_VERSION ||
        context->struct_size != sizeof(*context) || context->runtime == NULL ||
        context->model_format != MTFS_SENTINEL_MODEL_FORMAT_CPU_TFLITE_INT8_V2 ||
        validate_cpu_model(context->runtime, context->runtime_size) != MTFS_OK ||
        work_size < MTFS_SENTINEL_CPU_WORK_SIZE)
        return MTFS_ERROR_INVALID_STATE;
    if (ranges_overlap(input, 24U, output, 24U) ||
        ranges_overlap(input, 24U, work, MTFS_SENTINEL_CPU_WORK_SIZE) ||
        ranges_overlap(output, 24U, work, MTFS_SENTINEL_CPU_WORK_SIZE))
        return MTFS_ERROR_INVALID_ARGUMENT;
    model = context->runtime + le32(context->runtime + 160U);
    current = (int8_t *)storage; next = (int8_t *)(storage + 24U);
    (void)memcpy(current, input, 24U);
    for (layer_index = 0U; layer_index < 4U; ++layer_index) {
        const uint8_t *layer = model + MTFS_SENTINEL_CPU_TFLITE_INT8_HEADER_SIZE +
            layer_index * MTFS_SENTINEL_CPU_TFLITE_LAYER_DESCRIPTOR_SIZE;
        uint32_t input_count = dimensions[layer_index];
        uint32_t output_count = dimensions[layer_index + 1U];
        const uint8_t *weights = model + le32(layer + 16U);
        const uint8_t *biases = model + le32(layer + 20U);
        const uint8_t *weight_zeros = model + le32(layer + 28U);
        const uint8_t *multipliers = model + le32(layer + 32U);
        const uint8_t *shifts = model + le32(layer + 36U);
        int32_t input_zero = (int8_t)layer[6U];
        int32_t output_zero = (int8_t)layer[7U];
        uint32_t o, n;
        for (o = 0U; o < output_count; ++o) {
            int64_t accumulator = le_i32(biases + o * 4U);
            int32_t quantized;
            int32_t weight_zero = (int8_t)weight_zeros[o];
            for (n = 0U; n < input_count; ++n)
                accumulator += ((int32_t)current[n] - input_zero) *
                    ((int32_t)(int8_t)weights[o * input_count + n] - weight_zero);
            if (accumulator < INT32_MIN || accumulator > INT32_MAX)
                return MTFS_ERROR_OVERFLOW;
            if (tflite_multiply((int32_t)accumulator,
                    le_i32(multipliers + o * 4U), (int8_t)shifts[o],
                    &quantized) != MTFS_OK) return MTFS_ERROR_OVERFLOW;
            {
                int64_t adjusted = (int64_t)quantized + output_zero;
                if (layer[4U] == 1U && adjusted < output_zero) adjusted = output_zero;
                if (adjusted < -128) adjusted = -128;
                if (adjusted > 127) adjusted = 127;
                next[o] = (int8_t)adjusted;
            }
        }
        { int8_t *swap = current; current = next; next = swap; }
    }
    (void)memcpy(output, current, 24U);
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_cpu_infer_detailed(
    const mtfs_sentinel_cpu_context_t *context, const int8_t input[24],
    void *work, size_t work_size, int8_t raw_output_int8[24], int8_t output[24],
    mtfs_sentinel_inference_result_t *result)
{
    static const uint8_t dimensions[5] = {24U,12U,4U,12U,24U};
    uint8_t *storage = (uint8_t *)work;
    int8_t *current, *next;
    int8_t canonical_input[24], canonical_output[24];
    uint32_t layer, weight_offset = 0U, bias_offset = 0U;
    mtfs_error_t status;
    if (context == NULL || input == NULL || work == NULL || raw_output_int8 == NULL ||
        output == NULL || result == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    if (context->api_version != MTFS_SENTINEL_INFERENCE_API_VERSION ||
        context->struct_size != sizeof(*context) || context->runtime == NULL ||
        work_size < MTFS_SENTINEL_CPU_WORK_SIZE)
        return MTFS_ERROR_INVALID_STATE;
    if (ranges_overlap(input, 24U, output, 24U) ||
        ranges_overlap(input, 24U, raw_output_int8, 24U) ||
        ranges_overlap(raw_output_int8, 24U, output, 24U) ||
        ranges_overlap(raw_output_int8, 24U, work, MTFS_SENTINEL_CPU_WORK_SIZE) ||
        ranges_overlap(raw_output_int8, 24U, result, sizeof(*result)) ||
        ranges_overlap(input, 24U, work, MTFS_SENTINEL_CPU_WORK_SIZE) ||
        ranges_overlap(input, 24U, result, sizeof(*result)) ||
        ranges_overlap(output, 24U, work, MTFS_SENTINEL_CPU_WORK_SIZE) ||
        ranges_overlap(result, sizeof(*result), output, 24U) ||
        ranges_overlap(result, sizeof(*result), work, MTFS_SENTINEL_CPU_WORK_SIZE))
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (context->model_format == MTFS_SENTINEL_MODEL_FORMAT_CPU_TFLITE_INT8_V2) {
        for (layer = 0U; layer < 24U; ++layer)
            canonical_input[layer] = q4_to_canonical(input[layer],
                le32(context->runtime + 32U), le32(context->runtime + 36U),
                (int8_t)context->runtime[30U]);
        status = mtfs_sentinel_cpu_infer_canonical_int8(context, canonical_input,
            work, work_size, canonical_output);
        if (status != MTFS_OK) return status;
        (void)memcpy(raw_output_int8, canonical_output, 24U);
        for (layer = 0U; layer < 24U; ++layer)
            output[layer] = canonical_to_q4(canonical_output[layer],
                le32(context->runtime + 40U), le32(context->runtime + 44U),
                (int8_t)context->runtime[31U]);
        return mtfs_sentinel_score_q8(input, output, context->threshold_q8, result);
    }
    if (context->model_format != MTFS_SENTINEL_MODEL_FORMAT_CPU_INT8_V1)
        return MTFS_ERROR_INVALID_STATE;
    current = (int8_t *)storage;
    next = (int8_t *)(storage + 24U);
    (void)memcpy(current, input, 24U);
    for (layer = 0U; layer < 4U; ++layer) {
        uint32_t in_count = dimensions[layer], out_count = dimensions[layer + 1U];
        uint32_t o, n;
        for (o = 0U; o < out_count; ++o) {
            const uint8_t *model = context->runtime + le32(context->runtime + 160U);
            const uint8_t *weights = model + MTFS_SENTINEL_CPU_MODEL_BINARY_HEADER_SIZE;
            const uint8_t *biases = weights + MTFS_SENTINEL_CPU_WEIGHT_COUNT;
            int64_t accumulator = le_i32(biases + bias_offset + o * 4U);
            int32_t quantized;
            for (n = 0U; n < in_count; ++n) {
                int8_t weight = (int8_t)weights[
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
    status = mtfs_sentinel_score_q8(input, current, context->threshold_q8, result);
    if (status != MTFS_OK) return status;
    (void)memcpy(raw_output_int8, current, 24U);
    (void)memcpy(output, current, 24U);
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_cpu_infer(
    const mtfs_sentinel_cpu_context_t *context, const int8_t input[24],
    void *work, size_t work_size, int8_t output[24],
    mtfs_sentinel_inference_result_t *result)
{
    int8_t discarded_raw[24];
    mtfs_error_t status = mtfs_sentinel_cpu_infer_detailed(context, input,
        work, work_size, discarded_raw, output, result);
    (void)memset(discarded_raw, 0, sizeof(discarded_raw));
    return status;
}

mtfs_error_t mtfs_sentinel_score_q8(const int8_t input_q4[24],
    const int8_t output_q4[24], uint64_t threshold_q8,
    mtfs_sentinel_inference_result_t *result)
{
    mtfs_sentinel_inference_result_t completed;
    uint64_t sum = 0U;
    uint32_t i;
    if (input_q4 == NULL || output_q4 == NULL || result == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    for (i = 0U; i < 24U; ++i) {
        int32_t difference = (int32_t)input_q4[i] - output_q4[i];
        sum += (uint64_t)(difference * difference);
    }
    (void)memset(&completed, 0, sizeof(completed));
    completed.api_version = MTFS_SENTINEL_INFERENCE_API_VERSION;
    completed.struct_size = (uint16_t)sizeof(completed);
    completed.score_q8 = (sum + 12U) / 24U;
    completed.threshold_q8 = threshold_q8;
    completed.anomaly = completed.score_q8 > threshold_q8 ? 1U : 0U;
    *result = completed;
    return MTFS_OK;
}

mtfs_error_t mtfs_sentinel_score_interval_q8(const int8_t input_q4[24],
    const int8_t reference_output_q4[24], uint32_t maximum_q4_error,
    uint64_t threshold_q8, mtfs_sentinel_score_interval_t *interval)
{
    mtfs_sentinel_score_interval_t completed;
    uint64_t minimum_sum = 0U, maximum_sum = 0U;
    uint32_t i;
    if (input_q4 == NULL || reference_output_q4 == NULL || interval == NULL ||
        maximum_q4_error > 255U)
        return MTFS_ERROR_INVALID_ARGUMENT;
    for (i = 0U; i < 24U; ++i) {
        int32_t input = input_q4[i];
        int32_t reference = reference_output_q4[i];
        int32_t lower = reference - (int32_t)maximum_q4_error;
        int32_t upper = reference + (int32_t)maximum_q4_error;
        int32_t minimum_delta, lower_delta, upper_delta, maximum_delta;
        if (lower < -128) lower = -128;
        if (upper > 127) upper = 127;
        if (input < lower) minimum_delta = lower - input;
        else if (input > upper) minimum_delta = input - upper;
        else minimum_delta = 0;
        lower_delta = input - lower;
        if (lower_delta < 0) lower_delta = -lower_delta;
        upper_delta = input - upper;
        if (upper_delta < 0) upper_delta = -upper_delta;
        maximum_delta = lower_delta > upper_delta ? lower_delta : upper_delta;
        minimum_sum += (uint64_t)(minimum_delta * minimum_delta);
        maximum_sum += (uint64_t)(maximum_delta * maximum_delta);
    }
    (void)memset(&completed, 0, sizeof(completed));
    completed.api_version = MTFS_SENTINEL_INFERENCE_API_VERSION;
    completed.struct_size = (uint16_t)sizeof(completed);
    completed.score_min_q8 = (minimum_sum + 12U) / 24U;
    completed.score_max_q8 = (maximum_sum + 12U) / 24U;
    completed.threshold_q8 = threshold_q8;
    if (completed.score_max_q8 <= threshold_q8) {
        completed.decision_class = MTFS_SENTINEL_DECISION_DEFINITELY_NORMAL;
    } else if (completed.score_min_q8 > threshold_q8) {
        completed.decision_class = MTFS_SENTINEL_DECISION_DEFINITELY_ANOMALY;
    } else {
        completed.decision_class =
            MTFS_SENTINEL_DECISION_AMBIGUOUS_CPU_ARBITRATION;
    }
    *interval = completed;
    return MTFS_OK;
}

#else
typedef int mtfs_sentinel_inference_disabled_translation_unit_t;
#endif /* MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE */
