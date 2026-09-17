/** @file mtfs_sentinel_sealed_adapter.h
 * @brief Authenticated model payload to Sentinel bundle adapter. / 認証済みモデルペイロードを Sentinel bundle として扱うためのアダプター。
 * @ingroup mtfs_sentinel */
#ifndef MTFS_SENTINEL_SEALED_ADAPTER_H
#define MTFS_SENTINEL_SEALED_ADAPTER_H

/** @addtogroup mtfs_sentinel
 * @{ */

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
 * payload は呼び出し側が確保したメモリ領域の先頭に配置し、
 * mtfs_sentinel_bundle_memory_plan() が返すアライメント要件を満たしている必要があります。 
 * outer の required_ram には、メモリに常駐する payload に加えて、 
 * アライメントされた runtime persistent 領域および shared-scratch 領域に必要なサイズも含まれます。
 */

/** @brief Cross-check outer metadata and parse a resident inner bundle. / outer metadata との整合性を確認し、メモリ上に常駐している inner bundle を解析する。
 * @param payload Resident plaintext retained unchanged while bundle is used. / bundle の使用中、変更せずに保持される常駐 plaintext。
 * @param payload_size Resident payload bytes. / 常駐 payload のサイズ (バイト単位)。
 * @param outer_model Authenticated outer model metadata. / 認証済みの outer model metadata。
 * @param expected_transport_id Required inner transport id. / inner bundle に要求される transport ID。
 * @param expected_profile_id Required inner profile id. / inner bundle に要求される profile ID。
 * @param[out] bundle Borrowed parsed view. / payload 内を参照する解析済みビュー。payload の所有権は取得しない。
 * @return MTFS_OK or identity/size/format/state error. / MTFS_OK または identity/size/format/state error。 */
mtfs_error_t mtfs_sentinel_bundle_parse_model_payload(
    const void *payload, size_t payload_size,
    const mtfs_model_info_t *outer_model,
    uint32_t expected_transport_id, uint32_t expected_profile_id,
    mtfs_sentinel_bundle_t *bundle);

#ifdef __cplusplus
}
#endif

/** @} */
#endif
#endif
