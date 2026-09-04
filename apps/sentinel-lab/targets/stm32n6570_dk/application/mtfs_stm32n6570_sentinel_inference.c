#include "mtfs_stm32n6570_sentinel_inference.h"

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE && MTFS_ENABLE_SEALED_MODEL

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "ll_aton_NN_interface.h"
#include "mtfs_model_store.h"
#include "mtfs_sealed_format.h"
#include "mtfs_sealed_reader_fatfs.h"
#include "mtfs_secure_zero.h"
#include "mtfs_sentinel_inference.h"
#include "mtfs_sentinel_npu_provider.h"
#include "mtfs_sentinel_sealed_adapter.h"
#include "mtfs_sentinel_sha256.h"
#include "mtfs_stm32_saes_provider.h"
#include "mtfs_stm32n6570_dk_platform.h"
#include "mtfs_stm32n6570_nor.h"
#include "mtfs_stm32n6570_sentinel_npu.h"
#include "mtfs_stm32n6_neural_art.h"

#define SENTINEL_MODEL_PATH "0:/SENTINEL.MTF"
#define SENTINEL_PROFILE_ID (UINT32_C(0x2c9ec916))
#define SENTINEL_ARENA_SIZE (UINT32_C(32768))
#define SENTINEL_TIMEOUT_MS (UINT32_C(2000))
#define SENTINEL_HOTPLUG_WAIT_TICKS (UINT32_C(12000))
#define SENTINEL_MAX_OUTPUT_ERROR_Q4 (1)
#define SENTINEL_MAX_SCORE_ERROR_Q8 (UINT64_C(2))

#if defined(__GNUC__)
#define SENTINEL_ALIGN32 __attribute__((aligned(32)))
#else
#define SENTINEL_ALIGN32
#endif

typedef struct sentinel_session
{
    mtfs_sealed_reader_fatfs_t fatfs_reader;
    mtfs_sealed_reader_t reader;
    mtfs_stm32_saes_provider_context_t crypto_context;
    mtfs_crypto_provider_t crypto;
    mtfs_stm32_nor_io_t nor;
    mtfs_model_t model;
    mtfs_model_info_t model_info;
    mtfs_sentinel_bundle_t bundle;
    mtfs_sentinel_memory_plan_t plan;
    mtfs_sentinel_cpu_context_t cpu;
    mtfs_sentinel_npu_context_t npu;
    mtfs_stm32n6_neural_art_t neural_art;
    mtfs_sentinel_npu_provider_config_t npu_config;
    NN_Instance_TypeDef nn_instance;
    const mtfs_media_context_t *media;
    ID saes_mutex;
    ID npu_mutex;
    uint32_t npu_runtime_index;
    uint8_t reader_open;
    uint8_t crypto_open;
    uint8_t model_initialized;
    uint8_t npu_open;
} sentinel_session_t;

static uint8_t manifest_work[MTFS_SEALED_MAX_MANIFEST_SIZE] SENTINEL_ALIGN32;
static uint8_t aad_work[MTFS_SEALED_CHUNK_AAD_SIZE] SENTINEL_ALIGN32;
static uint8_t ciphertext_work[MTFS_SEALED_CIPHER_BUFFER_SIZE] SENTINEL_ALIGN32;
static uint8_t plaintext_work[MTFS_SEALED_MAX_CHUNK_SIZE] SENTINEL_ALIGN32;
static uint8_t arena[SENTINEL_ARENA_SIZE] SENTINEL_ALIGN32;

static mtfs_sealed_work_t sealed_work(void)
{
    mtfs_sealed_work_t work;
    work.api_version = MTFS_SEALED_WORK_API_VERSION;
    work.struct_size = (uint32_t)sizeof(work);
    work.manifest = manifest_work;
    work.manifest_capacity = sizeof(manifest_work);
    work.aad = aad_work;
    work.aad_capacity = sizeof(aad_work);
    work.ciphertext = ciphertext_work;
    work.ciphertext_capacity = sizeof(ciphertext_work);
    work.plaintext = plaintext_work;
    work.plaintext_capacity = sizeof(plaintext_work);
    return work;
}

static mtfs_model_policy_t model_policy(void)
{
    mtfs_model_policy_t policy;
    (void)memset(&policy, 0, sizeof(policy));
    policy.api_version = MTFS_MODEL_POLICY_API_VERSION;
    policy.struct_size = (uint32_t)sizeof(policy);
    policy.expected_target_id = MTFS_SENTINEL_TARGET_STM32N6570_DK;
    policy.expected_accelerator_id = MTFS_SENTINEL_ACCELERATOR_NEURAL_ART;
    policy.accepted_model_format = MTFS_MODEL_FORMAT_SENTINEL_BUNDLE_V1;
    policy.maximum_chunk_size = MTFS_SEALED_MAX_CHUNK_SIZE;
    policy.maximum_model_payload_size = MTFS_SENTINEL_BUNDLE_MAX_SIZE;
    policy.maximum_required_ram = SENTINEL_ARENA_SIZE;
    return policy;
}

static mtfs_error_t media_status(void *opaque)
{
    const mtfs_media_context_t *media = opaque;
    mtfs_media_state_t state;
    if (media == NULL) return MTFS_ERROR_NOT_READY;
    state = mtfs_media_state(media);
    if (state == MTFS_MEDIA_STATE_PRESENT) return MTFS_OK;
    return state == MTFS_MEDIA_STATE_ABSENT ? MTFS_ERROR_NO_MEDIA :
        MTFS_ERROR_NOT_READY;
}

static int saes_lock(void *opaque)
{
    sentinel_session_t *session = opaque;
    return session->saes_mutex > 0 &&
        tk_loc_mtx(session->saes_mutex, TMO_FEVR) == E_OK ? 0 : -1;
}

static void saes_unlock(void *opaque)
{
    sentinel_session_t *session = opaque;
    if (session->saes_mutex > 0) (void)tk_unl_mtx(session->saes_mutex);
}

static mtfs_error_t npu_lock(void *opaque, uint32_t timeout_ms)
{
    sentinel_session_t *session = opaque;
    TMO timeout = timeout_ms > (uint32_t)INT32_MAX ? TMO_FEVR : (TMO)timeout_ms;
    return session->npu_mutex > 0 && timeout_ms != 0U &&
        tk_loc_mtx(session->npu_mutex, timeout) == E_OK ? MTFS_OK :
        MTFS_ERROR_NOT_READY;
}

static void npu_unlock(void *opaque)
{
    sentinel_session_t *session = opaque;
    if (session->npu_mutex > 0) (void)tk_unl_mtx(session->npu_mutex);
}

static mtfs_error_t npu_hash(void *opaque, const void *bytes, uint32_t size,
    uint8_t digest[32])
{
    (void)opaque;
    return mtfs_sentinel_sha256(bytes, size, digest);
}

static uint64_t npu_clock_ms(void *opaque)
{
    (void)opaque;
    return mtfs_stm32n6570_dk_benchmark_clock_us(NULL) / UINT64_C(1000);
}

static void npu_yield(void *opaque)
{
    (void)opaque;
    (void)tk_dly_tsk(1U);
}

static int session_cleanup(sentinel_session_t *session)
{
    int failed = 0;
    mtfs_error_t status;
    if (session->npu_open != 0U) {
        status = mtfs_sentinel_npu_close(&session->npu, SENTINEL_TIMEOUT_MS);
        if (status != MTFS_OK) failed = 1;
        else session->npu_open = 0U;
    }
    if (session->model_initialized != 0U &&
        mtfs_model_close(&session->model) != MTFS_OK) failed = 1;
    session->model_initialized = 0U;
    if (session->reader_open != 0U) {
        status = mtfs_sealed_reader_fatfs_close(&session->fatfs_reader);
        if (status != MTFS_OK && status != MTFS_ERROR_NO_MEDIA &&
            status != MTFS_ERROR_NOT_READY) failed = 1;
    }
    session->reader_open = 0U;
    if (session->crypto_open != 0U &&
        mtfs_stm32_saes_provider_deinit(&session->crypto_context) !=
            MTFS_CRYPTO_OK) failed = 1;
    session->crypto_open = 0U;
    /* NPU close must succeed before its backing plaintext may be erased. */
    if (session->npu_open == 0U) mtfs_secure_zero(arena, sizeof(arena));
    mtfs_secure_zero(manifest_work, sizeof(manifest_work));
    mtfs_secure_zero(aad_work, sizeof(aad_work));
    mtfs_secure_zero(ciphertext_work, sizeof(ciphertext_work));
    mtfs_secure_zero(plaintext_work, sizeof(plaintext_work));
    if (session->npu_open == 0U) {
        if (session->npu_mutex > 0) (void)tk_del_mtx(session->npu_mutex);
        if (session->saes_mutex > 0) (void)tk_del_mtx(session->saes_mutex);
        session->npu_mutex = 0; session->saes_mutex = 0;
    }
    return failed;
}

static int session_open(sentinel_session_t *session)
{
    const mtfs_sentinel_runtime_region_policy_t *policies;
    mtfs_sentinel_runtime_info_t npu_runtime;
    mtfs_sealed_work_t work = sealed_work();
    mtfs_model_policy_t outer_policy = model_policy();
    T_CMTX mutex = {.mtxatr = TA_INHERIT};
    uint32_t policy_count;
    size_t loaded = 0U;
    int32_t bsp_error = 0;
    mtfs_error_t status;

    session->saes_mutex = tk_cre_mtx(&mutex);
    session->npu_mutex = tk_cre_mtx(&mutex);
    if (session->saes_mutex <= 0 || session->npu_mutex <= 0) return 1;
    mtfs_model_init(&session->model);
    session->model_initialized = 1U;
    mtfs_sealed_reader_fatfs_init(&session->fatfs_reader);
    status = mtfs_sealed_reader_fatfs_open(&session->fatfs_reader,
        SENTINEL_MODEL_PATH, media_status, (void *)session->media,
        &session->reader);
    if (status != MTFS_OK) return 2;
    session->reader_open = 1U;
    if (mtfs_stm32n6570_nor_open(&session->nor, &bsp_error) != 0) return 3;
    if (mtfs_stm32_saes_provider_init(&session->crypto_context, &session->nor,
            saes_lock, saes_unlock, session, &session->crypto) != MTFS_CRYPTO_OK)
        return 4;
    session->crypto_open = 1U;
    status = mtfs_model_open(&session->model, &session->reader,
        &session->crypto, &work, &outer_policy);
    if (status != MTFS_OK) return 5;
    if (mtfs_model_get_info(&session->model, &session->model_info) != MTFS_OK)
        return 6;
    (void)memset(arena, 0, sizeof(arena));
    status = mtfs_model_load(&session->model, arena, sizeof(arena), &loaded);
    if (status != MTFS_OK || loaded != session->model_info.payload_size) return 7;
    status = mtfs_sentinel_bundle_parse_model_payload(arena, loaded,
        &session->model_info, MTFS_SENTINEL_TRANSPORT_SDMMC_IDMA,
        SENTINEL_PROFILE_ID, &session->bundle);
    if (status != MTFS_OK) return 8;
    if (mtfs_sentinel_bundle_memory_plan(&session->bundle, &session->plan) !=
            MTFS_OK || session->plan.required_ram > sizeof(arena)) return 9;
    if (mtfs_sentinel_cpu_init(&session->cpu, &session->bundle) != MTFS_OK)
        return 10;
    if (mtfs_sentinel_bundle_runtime_find(&session->bundle,
            MTFS_SENTINEL_PROVIDER_ST_NEURAL_ART_RELOC,
            MTFS_SENTINEL_ACCELERATOR_NEURAL_ART, &npu_runtime) != MTFS_OK)
        return 11;
    session->npu_runtime_index = npu_runtime.runtime_index;
    if (mtfs_stm32n6570_sentinel_npu_policy(&policies, &policy_count) != MTFS_OK)
        return 12;
    if (mtfs_stm32n6_neural_art_provider_config(&session->neural_art,
            &session->nn_instance, npu_hash, npu_clock_ms, npu_yield,
            npu_lock, npu_unlock, session, &session->npu_config) != MTFS_OK)
        return 13;
    status = mtfs_sentinel_npu_open(&session->npu, &session->npu_config,
        &session->bundle, session->npu_runtime_index,
        arena + (size_t)session->plan.persistent_offset[
            session->npu_runtime_index],
        session->plan.persistent_size[session->npu_runtime_index], policies,
        policy_count, SENTINEL_TIMEOUT_MS);
    if (status != MTFS_OK) {
        tm_printf((UB *)"[sentinel-infer] NPU-open status=%d substage=%u detail=%d expected=0x%08x actual=0x%08x\n",
            status, (UW)session->neural_art.diagnostic_stage,
            session->neural_art.diagnostic_detail,
            (UW)session->neural_art.diagnostic_expected,
            (UW)session->neural_art.diagnostic_actual);
        return 14;
    }
    session->npu_open = 1U;
    return 0;
}

static int wait_media(const mtfs_media_context_t *media, int present)
{
    uint32_t count;
    for (count = 0U; count < SENTINEL_HOTPLUG_WAIT_TICKS; ++count) {
        if (!!mtfs_media_is_present(media) == !!present) return 0;
        (void)tk_dly_tsk(10U);
    }
    return 1;
}

static int inference_run(
    const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations,
    int hotplug)
{
    static sentinel_session_t session;
    uint32_t raw[MTFS_SENTINEL_FEATURE_DIMENSION];
    int8_t input[MTFS_SENTINEL_FEATURE_DIMENSION];
    int8_t cpu_output[MTFS_SENTINEL_FEATURE_DIMENSION];
    int8_t npu_output[MTFS_SENTINEL_FEATURE_DIMENSION];
    mtfs_sentinel_inference_result_t cpu_result, npu_result;
    uint64_t cpu_total = 0U, npu_total = 0U, start, elapsed;
    uint64_t score_error;
    uint32_t i, j, max_error = 0U;
    int stage, cleanup_failed;

    if (media == NULL || feature == NULL || iterations == 0U) return 1;
    if (session.npu_open != 0U && session_cleanup(&session) != 0) {
        tm_printf((UB *)"[sentinel-infer] FAIL prior NPU cleanup still pending\n");
        return 1;
    }
    (void)memset(&session, 0, sizeof(session));
    (void)memset(&cpu_result, 0, sizeof(cpu_result));
    (void)memset(&npu_result, 0, sizeof(npu_result));
    session.media = media;
    stage = session_open(&session);
    if (stage != 0) {
        cleanup_failed = session_cleanup(&session);
        tm_printf((UB *)"[sentinel-infer] FAIL open-stage=%d cleanup=%s\n",
            stage, cleanup_failed ? (UB *)"FAIL" : (UB *)"PASS");
        return 1;
    }
    if (hotplug) {
        tm_printf((UB *)"[sentinel-infer-hotplug] ACTION REQUIRED: REMOVE card; resident CPU/NPU inference will continue\n");
        if (wait_media(media, 0) != 0) stage = 19;
    }
    if (mtfs_sentinel_feature_encode_raw(feature, raw) != MTFS_OK ||
        mtfs_sentinel_normalize_int8(&session.bundle.normalization, raw,
            input) != MTFS_OK) stage = 15;
    for (i = 0U; stage == 0 && i < iterations; ++i) {
        start = mtfs_stm32n6570_dk_benchmark_clock_us(NULL);
        if (mtfs_sentinel_cpu_infer(&session.cpu, input,
                arena + session.plan.scratch_offset, session.plan.scratch_size,
                cpu_output, &cpu_result) != MTFS_OK) { stage = 16; break; }
        elapsed = mtfs_stm32n6570_dk_benchmark_clock_us(NULL) - start;
        cpu_total += elapsed;
        start = mtfs_stm32n6570_dk_benchmark_clock_us(NULL);
        if (mtfs_sentinel_npu_infer(&session.npu, input, npu_output,
                SENTINEL_TIMEOUT_MS, &npu_result) != MTFS_OK) {
            stage = 17; break;
        }
        elapsed = mtfs_stm32n6570_dk_benchmark_clock_us(NULL) - start;
        npu_total += elapsed;
        for (j = 0U; j < MTFS_SENTINEL_FEATURE_DIMENSION; ++j) {
            int delta = (int)cpu_output[j] - (int)npu_output[j];
            uint32_t absolute = (uint32_t)(delta < 0 ? -delta : delta);
            if (absolute > max_error) max_error = absolute;
        }
        score_error = cpu_result.score_q8 > npu_result.score_q8 ?
            cpu_result.score_q8 - npu_result.score_q8 :
            npu_result.score_q8 - cpu_result.score_q8;
        if (max_error > SENTINEL_MAX_OUTPUT_ERROR_Q4 ||
            score_error > SENTINEL_MAX_SCORE_ERROR_Q8 ||
            cpu_result.anomaly != npu_result.anomaly) { stage = 18; break; }
    }
    cleanup_failed = session_cleanup(&session);
    tm_printf((UB *)"[sentinel-infer] %s iterations=%u cpu-score-q8=%u npu-score-q8=%u decision=%u/%u max-output-error-q4=%u cpu-us=%u npu-us=%u required-ram=%u cleanup=%s\n",
        stage == 0 && !cleanup_failed ? (UB *)"PASS" : (UB *)"FAIL",
        i, (UW)cpu_result.score_q8, (UW)npu_result.score_q8,
        cpu_result.anomaly, npu_result.anomaly, max_error,
        i == 0U ? 0U : (UW)(cpu_total / i),
        i == 0U ? 0U : (UW)(npu_total / i),
        (UW)session.plan.required_ram,
        cleanup_failed ? (UB *)"FAIL" : (UB *)"PASS");
    if (hotplug) {
        tm_printf((UB *)"[sentinel-infer-hotplug] ACTION REQUIRED: REINSERT card\n");
        if (wait_media(media, 1) != 0) stage = 20;
    }
    return stage == 0 && !cleanup_failed ? 0 : 1;
}

int mtfs_stm32n6570_sentinel_inference_run(
    const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations)
{
    return inference_run(media, feature, iterations, 0);
}

int mtfs_stm32n6570_sentinel_inference_hotplug_run(
    const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations)
{
    return inference_run(media, feature, iterations, 1);
}

#else

int mtfs_stm32n6570_sentinel_inference_run(
    const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations)
{
    (void)media; (void)feature; (void)iterations;
    return 1;
}

int mtfs_stm32n6570_sentinel_inference_hotplug_run(
    const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations)
{
    (void)media; (void)feature; (void)iterations;
    return 1;
}

#endif
