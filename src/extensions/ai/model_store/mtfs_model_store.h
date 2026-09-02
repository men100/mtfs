#ifndef MTFS_MODEL_STORE_H
#define MTFS_MODEL_STORE_H

#include "mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL
#include <stddef.h>
#include <stdint.h>
#include "extensions/security/sealed_blob/mtfs_sealed_blob.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_MODEL_API_VERSION (1U)
#define MTFS_MODEL_POLICY_API_VERSION (1U)
#define MTFS_MODEL_INFO_API_VERSION (1U)

/* Registry rule: zero is invalid in every namespace and is never a wildcard. */
#define MTFS_MODEL_ID_INVALID (0U)
/* FourCC "SNT1": authenticated plaintext is Sentinel bundle v1. */
#define MTFS_MODEL_FORMAT_SENTINEL_BUNDLE_V1 (UINT32_C(0x534e5431))
/* Outer accelerator policy ID for a CPU-only Sentinel bundle. */
#define MTFS_ACCELERATOR_CPU_REFERENCE (UINT32_C(0x43505520))

typedef enum mtfs_model_state
{
    MTFS_MODEL_CLOSED = 0,
    MTFS_MODEL_OPENING = 1,
    MTFS_MODEL_OPEN = 2,
    MTFS_MODEL_LOADING = 3,
    MTFS_MODEL_LOADED = 4,
    MTFS_MODEL_ERROR = 5
} mtfs_model_state_t;

typedef struct mtfs_model_policy
{
    uint32_t api_version;
    uint32_t struct_size;
    uint32_t expected_target_id;
    uint32_t expected_accelerator_id;
    uint32_t accepted_model_format;
    uint32_t maximum_chunk_size;
    uint64_t maximum_model_payload_size;
    uint64_t maximum_required_ram;
} mtfs_model_policy_t;

typedef struct mtfs_model_info
{
    uint32_t api_version;
    uint32_t struct_size;
    uint8_t model_id[16];
    uint64_t model_version;
    uint32_t target_id;
    uint32_t accelerator_id;
    uint32_t model_format;
    uint64_t payload_size;
    uint64_t required_ram;
    uint32_t chunk_size;
    uint32_t chunk_count;
} mtfs_model_info_t;

typedef struct mtfs_model
{
    uint32_t api_version;
    uint32_t struct_size;
    mtfs_model_state_t state;
    mtfs_sealed_blob_t sealed_blob;
    mtfs_model_info_t info;
} mtfs_model_t;

void mtfs_model_init(mtfs_model_t *model);
mtfs_error_t mtfs_model_open(mtfs_model_t *model,
    const mtfs_sealed_reader_t *reader, const mtfs_crypto_provider_t *provider,
    const mtfs_sealed_work_t *work, const mtfs_model_policy_t *policy);
mtfs_error_t mtfs_model_get_info(const mtfs_model_t *model,
    mtfs_model_info_t *info);
mtfs_error_t mtfs_model_load(mtfs_model_t *model, void *destination,
    size_t destination_size, size_t *loaded_size);
mtfs_error_t mtfs_model_close(mtfs_model_t *model);

#ifdef __cplusplus
}
#endif
#endif /* MTFS_ENABLE_SEALED_MODEL */

#endif /* MTFS_MODEL_STORE_H */
