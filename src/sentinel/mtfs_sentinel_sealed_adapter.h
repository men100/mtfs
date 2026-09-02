/* Optional outer sealed-model to inner Sentinel bundle policy adapter. */
#ifndef MTFS_SENTINEL_SEALED_ADAPTER_H
#define MTFS_SENTINEL_SEALED_ADAPTER_H

#include "../mtfs_config.h"

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE && MTFS_ENABLE_SEALED_MODEL
#include <stddef.h>
#include <stdint.h>
#include "../extensions/ai/model_store/mtfs_model_store.h"
#include "mtfs_sentinel_inference.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * payload is the start of the caller allocation and must meet the alignment
 * returned by mtfs_sentinel_bundle_memory_plan(). outer required_ram covers
 * the resident payload plus aligned runtime persistent/shared-scratch areas.
 */
mtfs_error_t mtfs_sentinel_bundle_parse_model_payload(
    const void *payload, size_t payload_size,
    const mtfs_model_info_t *outer_model,
    uint32_t expected_transport_id, uint32_t expected_profile_id,
    mtfs_sentinel_bundle_t *bundle);

#ifdef __cplusplus
}
#endif
#endif
#endif
