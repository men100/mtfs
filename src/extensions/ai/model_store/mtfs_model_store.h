/** @file mtfs_model_store.h
 * @brief Policy-checked sealed model lifecycle. / policy検証付きsealed modelのlifecycle管理。
 * @ingroup mtfs_model */
#ifndef MTFS_MODEL_STORE_H
#define MTFS_MODEL_STORE_H

/** @addtogroup mtfs_model
 * @{ */

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
/* Registry規則: 0はすべてのnamespaceで無効であり、wildcardとしても扱わない。 */
#define MTFS_MODEL_ID_INVALID (0U)

/* FourCC "SNT1": authenticated plaintext is Sentinel bundle v1. */
/* FourCC "SNT1": 認証済みplaintextがSentinel bundle v1であることを示す。 */
#define MTFS_MODEL_FORMAT_SENTINEL_BUNDLE_V1 (UINT32_C(0x534e5431))

/* Outer accelerator policy ID for a CPU-only Sentinel bundle. */
/* CPU-only Sentinel bundleに対して指定する、outer側のaccelerator policy ID。 */
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

/** @brief Initialize a caller-owned closed model. / 呼び出し側が所有するmodelをCLOSED状態で初期化する。
 * @param model Storage to initialize. / 初期化対象のstorage。 */
void mtfs_model_init(mtfs_model_t *model);

/** @brief Authenticate package metadata and enforce model policy. / package metadataを認証し、model policyに適合することを検証する。
 * @param model Initialized model object. / 初期化済みmodel object。
 * @param reader Borrowed reader retained until close. / closeまで有効な状態を維持する非所有のreader。
 * @param provider Borrowed crypto provider retained until close. / closeまで有効な状態を維持する非所有のcrypto provider。
 * @param work Caller-owned sensitive work buffers. / 呼び出し側が所有する機密情報用work buffer。
 * @param policy Expected target, accelerator, format, and size bounds. / 期待するtarget、accelerator、model format、およびsize上限を定義するpolicy。
 * @return MTFS_OK or state, authentication, format, policy, or provider error. / MTFS_OKまたはstate/認証/format/policy/provider error。 */
mtfs_error_t mtfs_model_open(mtfs_model_t *model,
    const mtfs_sealed_reader_t *reader, const mtfs_crypto_provider_t *provider,
    const mtfs_sealed_work_t *work, const mtfs_model_policy_t *policy);

/** @brief Copy authenticated model metadata. / 認証済みmodel metadataをコピーする。
 * @param model Successfully opened or loaded model. / openまたはloadに成功したmodel。
 * @param[out] info Caller output. / 呼び出し側が用意する出力先。
 * @return MTFS_OK or state/argument error. / MTFS_OKまたはstate/引数error。 */
mtfs_error_t mtfs_model_get_info(const mtfs_model_t *model,
    mtfs_model_info_t *info);

/** @brief Load authenticated plaintext into resident model RAM. / 認証済みplaintextをmodel用の常駐RAMへloadする。
 * @param model Successfully opened model. / openに成功したmodel。
 * @param[out] destination Caller-owned RAM kept alive while the runtime uses it. / runtimeが使用している間、有効な状態を維持する呼び出し側所有のRAM。
 * @param destination_size Available bytes satisfying policy and runtime alignment. / policy上のsize要件とruntimeのalignment要件を満たす、destinationの利用可能サイズ。
 * @param[out] loaded_size Payload bytes on success, zero on entry. / 成功時にloadしたpayloadのサイズ（byte単位）を返す。
 * @return MTFS_OK or sealed-blob/size/state error. / MTFS_OKまたはsealed blob/size/state error。 */
mtfs_error_t mtfs_model_load(mtfs_model_t *model, void *destination,
    size_t destination_size, size_t *loaded_size);

/** @brief Close the sealed package and clear model metadata. / sealed packageをcloseし、model metadataを消去する。
 * @param model Initialized model object. / 初期化済みmodel object。
 * @return MTFS_OK or provider close error. / MTFS_OKまたはprovider close error。
 * @note The caller remains responsible for zeroizing successfully loaded resident RAM after runtime use. / loadに成功した常駐RAMは、runtimeでの使用終了後に呼び出し側がzeroizeする責任を持つ。 */
mtfs_error_t mtfs_model_close(mtfs_model_t *model);

#ifdef __cplusplus
}
#endif
#endif /* MTFS_ENABLE_SEALED_MODEL */

/** @} */

#endif /* MTFS_MODEL_STORE_H */
