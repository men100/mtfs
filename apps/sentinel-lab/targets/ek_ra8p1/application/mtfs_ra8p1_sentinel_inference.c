#include "mtfs_ra8p1_sentinel_inference.h"

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE && MTFS_ENABLE_SEALED_MODEL

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "extensions/ai/model_store/mtfs_model_store.h"
#include "mtfs_ra8p1_ethosu_hooks.h"
#include "mtfs_ra8p1_platform.h"
#include "ports/ra_fsp/crypto/mtfs_ra8p1_rsip_provider.h"
#include "mtfs_ra8p1_sentinel_npu.h"
#include "mtfs_ra8p1_sentinel_runtime.h"
#include "mtfs_ra8p1_tflm_ethosu.h"
#include "extensions/security/sealed_blob/mtfs_sealed_format.h"
#include "mtfs_sealed_reader_fatfs.h"
#include "extensions/security/sealed_blob/mtfs_secure_zero.h"
#include "mtfs_sentinel_inference.h"
#include "mtfs_sentinel_npu_provider.h"
#include "mtfs_sentinel_sealed_adapter.h"

#define SENTINEL_MODEL_PATH "0:/SENTINEL.MTF"
#define SENTINEL_PROFILE_ID (UINT32_C(0x30cdf67d))
#define SENTINEL_ARENA_SIZE (UINT32_C(32768))
#define SENTINEL_TIMEOUT_MS (UINT32_C(2000))
#define SENTINEL_HOTPLUG_WAIT_TICKS (UINT32_C(12000))
#define SENTINEL_MAX_OUTPUT_ERROR_Q4 (0U)
#define SENTINEL_MAX_RAW_OUTPUT_ERROR_INT8 (0U)

#if defined(__GNUC__)
#define SENTINEL_ALIGN32 __attribute__((aligned(32)))
#else
#define SENTINEL_ALIGN32
#endif

typedef struct sentinel_session
{
    mtfs_sealed_reader_fatfs_t fatfs_reader;
    mtfs_sealed_reader_t reader;
    mtfs_ra8p1_rsip_provider_context_t crypto_context;
    mtfs_crypto_provider_t crypto;
    mtfs_model_t model;
    mtfs_model_info_t model_info;
    mtfs_sentinel_bundle_t bundle;
    mtfs_sentinel_memory_plan_t plan;
    mtfs_sentinel_cpu_context_t cpu;
    mtfs_sentinel_npu_context_t npu;
    mtfs_ra8p1_tflm_ethosu_t ethosu;
    mtfs_sentinel_npu_provider_config_t npu_config;
    const mtfs_media_context_t *media;
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
static sentinel_session_t monitor_session;

extern uint32_t mtfs_ra8p1_heap_call_count(void);

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
    policy.expected_target_id = MTFS_SENTINEL_TARGET_EK_RA8P1;
    policy.expected_accelerator_id = MTFS_SENTINEL_ACCELERATOR_ETHOS_U55;
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

static uint32_t diagnostic_microseconds(void *opaque)
{
    (void)opaque;
    return (uint32_t)mtfs_ra8p1_benchmark_clock_us(NULL);
}

static int session_cleanup(sentinel_session_t *session)
{
    int failed = 0;
    mtfs_error_t status;
    if (session->npu_open != 0U) {
        status = mtfs_sentinel_npu_close(&session->npu,
            SENTINEL_TIMEOUT_MS);
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
        mtfs_ra8p1_rsip_provider_deinit(&session->crypto_context) !=
            MTFS_CRYPTO_OK) failed = 1;
    session->crypto_open = 0U;
    /* Never erase authenticated backing before the provider releases it. */
    if (session->npu_open == 0U) mtfs_secure_zero(arena, sizeof(arena));
    mtfs_secure_zero(manifest_work, sizeof(manifest_work));
    mtfs_secure_zero(aad_work, sizeof(aad_work));
    mtfs_secure_zero(ciphertext_work, sizeof(ciphertext_work));
    mtfs_secure_zero(plaintext_work, sizeof(plaintext_work));
    return failed;
}

static int session_open(sentinel_session_t *session)
{
    const mtfs_sentinel_runtime_region_policy_t *policies;
    mtfs_sentinel_runtime_info_t npu_runtime;
    mtfs_sealed_work_t work = sealed_work();
    mtfs_model_policy_t outer_policy = model_policy();
    uint32_t policy_count;
    uint32_t lock_attempts, unlock_attempts;
    int32_t last_lock_status, last_unlock_status;
    mtfs_crypto_status_t crypto_status;
    size_t loaded = 0U;
    mtfs_error_t status;

    mtfs_model_init(&session->model);
    session->model_initialized = 1U;
    mtfs_sealed_reader_fatfs_init(&session->fatfs_reader);
    status = mtfs_sealed_reader_fatfs_open(&session->fatfs_reader,
        SENTINEL_MODEL_PATH, media_status, (void *)session->media,
        &session->reader);
    if (status != MTFS_OK) return 1;
    session->reader_open = 1U;
    crypto_status = mtfs_ra8p1_rsip_provider_init(&session->crypto_context,
        mtfs_ra8p1_sentinel_rsip_lock,
        mtfs_ra8p1_sentinel_rsip_unlock, NULL, &session->crypto);
    if (crypto_status != MTFS_CRYPTO_OK) {
        mtfs_ra8p1_sentinel_rsip_lock_diagnostics(&last_lock_status,
            &last_unlock_status, &lock_attempts, &unlock_attempts);
        tm_printf((UB *)"[sentinel-infer] provider-init status=%d rsip-lock=%d rsip-unlock=%d locks=%u unlocks=%u\n",
            crypto_status, last_lock_status, last_unlock_status,
            lock_attempts, unlock_attempts);
        return 2;
    }
    session->crypto_open = 1U;
    status = mtfs_model_open(&session->model, &session->reader,
        &session->crypto, &work, &outer_policy);
    if (status != MTFS_OK) {
        mtfs_ra8p1_sentinel_rsip_lock_diagnostics(&last_lock_status,
            &last_unlock_status, &lock_attempts, &unlock_attempts);
        tm_printf((UB *)"[sentinel-infer] model-open status=%d provider-stage=%u provider-psa=%d provider-fsp=%d store=%d rsip-lock=%d rsip-unlock=%d locks=%u unlocks=%u\n",
            status, (UW)session->crypto_context.last_open_stage,
            session->crypto_context.last_open_psa_status,
            session->crypto_context.last_open_fsp_status,
            session->crypto_context.store_diagnostics.last_status,
            last_lock_status, last_unlock_status, lock_attempts,
            unlock_attempts);
        return 3;
    }
    if (mtfs_model_get_info(&session->model, &session->model_info) != MTFS_OK)
        return 4;
    (void)memset(arena, 0, sizeof(arena));
    status = mtfs_model_load(&session->model, arena, sizeof(arena), &loaded);
    if (status != MTFS_OK || loaded != session->model_info.payload_size) {
        mtfs_ra8p1_sentinel_rsip_lock_diagnostics(&last_lock_status,
            &last_unlock_status, &lock_attempts, &unlock_attempts);
        tm_printf((UB *)"[sentinel-infer] model-load status=%d loaded=%u expected=%u required-ram=%u capacity=%u psa=%d fsp=%d decrypt-stage=%u decrypt-psa=%d decrypt-cipher=%u decrypt-aad=%u decrypt-out=%u decrypts=%u rsip-lock=%d rsip-unlock=%d locks=%u unlocks=%u\n",
            status, (UW)loaded, (UW)session->model_info.payload_size,
            (UW)session->model_info.required_ram, (UW)sizeof(arena),
            session->crypto_context.last_psa_status,
            session->crypto_context.last_fsp_status,
            (UW)session->crypto_context.last_decrypt_stage,
            session->crypto_context.last_decrypt_psa_status,
            (UW)session->crypto_context.last_decrypt_ciphertext_size,
            (UW)session->crypto_context.last_decrypt_aad_size,
            (UW)session->crypto_context.last_decrypt_output_size,
            (UW)session->crypto_context.decrypt_attempts, last_lock_status,
            last_unlock_status, lock_attempts, unlock_attempts);
        return 5;
    }
    status = mtfs_sealed_reader_fatfs_close(&session->fatfs_reader);
    if (status != MTFS_OK) return 6;
    session->reader_open = 0U;
    (void)memset(&session->reader, 0, sizeof(session->reader));
    status = mtfs_sentinel_bundle_parse_model_payload(arena, loaded,
        &session->model_info, MTFS_SENTINEL_TRANSPORT_SPI,
        SENTINEL_PROFILE_ID, &session->bundle);
    if (status != MTFS_OK) return 7;
    if (mtfs_sentinel_bundle_memory_plan(&session->bundle, &session->plan) !=
            MTFS_OK || session->plan.required_ram > sizeof(arena)) return 8;
    if (mtfs_sentinel_cpu_init(&session->cpu, &session->bundle) != MTFS_OK)
        return 9;
    if (mtfs_sentinel_bundle_runtime_find(&session->bundle,
            MTFS_SENTINEL_PROVIDER_RA_TFLM_ETHOSU,
            MTFS_SENTINEL_ACCELERATOR_ETHOS_U55, &npu_runtime) != MTFS_OK)
        return 10;
    session->npu_runtime_index = npu_runtime.runtime_index;
    if (mtfs_ra8p1_sentinel_npu_policy(&policies, &policy_count) != MTFS_OK)
        return 11;
    if (mtfs_ra8p1_tflm_ethosu_provider_config(&session->ethosu,
            mtfs_ra8p1_sentinel_npu_lock,
            mtfs_ra8p1_sentinel_npu_unlock, NULL,
            &session->npu_config) != MTFS_OK) return 12;
    status = mtfs_sentinel_npu_open(&session->npu, &session->npu_config,
        &session->bundle, session->npu_runtime_index,
        arena + (size_t)session->plan.persistent_offset[
            session->npu_runtime_index],
        session->plan.persistent_size[session->npu_runtime_index], policies,
        policy_count, SENTINEL_TIMEOUT_MS);
    if (status != MTFS_OK) {
        tm_printf((UB *)"[sentinel-infer] NPU-open status=%d substage=%u detail=%d expected=0x%08x actual=0x%08x\n",
            status, session->ethosu.diagnostics.diagnostic_stage,
            session->ethosu.diagnostics.diagnostic_detail,
            session->ethosu.diagnostics.diagnostic_expected,
            session->ethosu.diagnostics.diagnostic_actual);
        return 13;
    }
    session->npu_open = 1U;
    tm_printf((UB *)"[sentinel-model] authenticated=PASS target=EK-RA8P1 accelerator=Ethos-U55 payload=%u required-ram=%u runtime-direct=PASS arena-used=%u\n",
        (UW)loaded, (UW)session->plan.required_ram,
        session->ethosu.diagnostics.arena_used);
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

static uint32_t average_u64(uint64_t total, uint32_t count)
{
    return count == 0U ? 0U : (uint32_t)(total / count);
}

static void profile_accumulate(mtfs_sentinel_npu_inference_profile_t *total,
    const mtfs_sentinel_npu_inference_profile_t *sample)
{
    total->input_requantize_cycles += sample->input_requantize_cycles;
    total->target_infer_cycles += sample->target_infer_cycles;
    total->output_requantize_cycles += sample->output_requantize_cycles;
    total->score_decision_cycles += sample->score_decision_cycles;
    total->total_cycles += sample->total_cycles;
    total->attempted += sample->attempted;
    total->completed += sample->completed;
}

static int inference_run(const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, const int8_t supplied_input[24],
    uint32_t iterations, int hotplug, int diagnostics,
    mtfs_ra8p1_sentinel_media_transition_fn transition,
    void *transition_context)
{
    sentinel_session_t *session = &monitor_session;
    uint32_t raw[MTFS_SENTINEL_FEATURE_DIMENSION];
    int8_t input[MTFS_SENTINEL_FEATURE_DIMENSION];
    int8_t cpu_output[MTFS_SENTINEL_FEATURE_DIMENSION];
    int8_t npu_output[MTFS_SENTINEL_FEATURE_DIMENSION];
    int8_t cpu_raw_output[MTFS_SENTINEL_FEATURE_DIMENSION];
    int8_t npu_raw_output[MTFS_SENTINEL_FEATURE_DIMENSION];
    mtfs_sentinel_inference_result_t cpu_result, npu_result;
    mtfs_sentinel_score_interval_t score_interval;
    mtfs_sentinel_npu_inference_profile_t provider_profile, provider_sample;
    mtfs_ra8p1_ethosu_hook_diagnostics_t hook_diagnostics;
    mtfs_ra8p1_tflm_ethosu_diagnostics_t target_diagnostics;
    uint64_t cpu_total_us = 0U, npu_total_us = 0U, score_error = 0U;
    uint32_t i, j, max_error = 0U, max_raw_error = 0U;
    uint32_t attempted = 0U, completed = 0U, failed = 0U, accepted = 0U;
    uint32_t raw_error_index = 0U, q4_error_index = 0U;
    uint32_t ambiguous = 0U, cpu_arbitrated = 0U;
    uint32_t heap_before, heap_after, required_ram = 0U;
    uint32_t rsip_capacity;
    uint64_t start_us, end_us;
    int rsip_memory_verified;
    int stage = 0, cleanup_failed, media_paused = 0, media_resumed = 0;
    mtfs_error_t infer_status;

    if (media == NULL || (feature == NULL && supplied_input == NULL) ||
        (feature != NULL && supplied_input != NULL) || iterations == 0U)
        return 1;
    if (hotplug && transition == NULL) return 1;
    if (session->npu_open != 0U && session_cleanup(session) != 0) {
        tm_printf((UB *)"[sentinel-infer] FAIL prior NPU cleanup still pending\n");
        return 1;
    }
    (void)memset(session, 0, sizeof(*session));
    (void)memset(&cpu_result, 0, sizeof(cpu_result));
    (void)memset(&npu_result, 0, sizeof(npu_result));
    (void)memset(&score_interval, 0, sizeof(score_interval));
    (void)memset(&provider_profile, 0, sizeof(provider_profile));
    (void)memset(&hook_diagnostics, 0, sizeof(hook_diagnostics));
    session->media = media;
    heap_before = mtfs_ra8p1_heap_call_count();
    if (diagnostics) mtfs_ra8p1_ethosu_hook_diagnostics_reset();
    stage = session_open(session);
    if (stage != 0) {
        cleanup_failed = session_cleanup(session);
        tm_printf((UB *)"[sentinel-infer] FAIL open-stage=%d cleanup=%s\n",
            stage, cleanup_failed ? (UB *)"FAIL" : (UB *)"PASS");
        return 1;
    }
    required_ram = session->plan.required_ram;
    if (hotplug) {
        tm_printf((UB *)"[sentinel-infer-hotplug] ACTION REQUIRED: REMOVE card; resident model retained and inference paused\n");
        if (wait_media(media, 0) != 0) stage = 14;
        else {
            media_paused = 1;
            if (transition(transition_context, 0) != MTFS_OK) stage = 22;
            else {
                tm_printf((UB *)"[sentinel-infer-hotplug] state=NO_MEDIA source=RULE inference=STOPPED unmount=PASS resident-model=retained\n");
                tm_printf((UB *)"[sentinel-infer-hotplug] ACTION REQUIRED: REINSERT card\n");
                if (wait_media(media, 1) != 0) stage = 15;
                else if (transition(transition_context, 1) != MTFS_OK)
                    stage = 23;
                else {
                    media_resumed = 1;
                    tm_printf((UB *)"[sentinel-infer-hotplug] media=PRESENT remount=PASS resident-model=retained inference=RESUMING\n");
                }
            }
        }
    }
    if (supplied_input != NULL) {
        (void)memcpy(input, supplied_input, sizeof(input));
    } else if (stage == 0 &&
        (mtfs_sentinel_feature_encode_raw(feature, raw) != MTFS_OK ||
         mtfs_sentinel_normalize_int8(&session->bundle.normalization, raw,
             input) != MTFS_OK)) {
        stage = 16;
    }
    for (i = 0U; stage == 0 && i < iterations; ++i) {
        ++attempted;
        start_us = mtfs_ra8p1_benchmark_clock_us(NULL);
        if (mtfs_sentinel_cpu_infer_detailed(&session->cpu, input,
                arena + session->plan.scratch_offset, session->plan.scratch_size,
                cpu_raw_output, cpu_output, &cpu_result) != MTFS_OK) {
            stage = 17; ++failed; break;
        }
        end_us = mtfs_ra8p1_benchmark_clock_us(NULL);
        cpu_total_us += end_us - start_us;
        start_us = mtfs_ra8p1_benchmark_clock_us(NULL);
        if (diagnostics) {
            infer_status = mtfs_sentinel_npu_infer_profiled_detailed(
                &session->npu, input, npu_raw_output, npu_output,
                SENTINEL_TIMEOUT_MS, &npu_result, diagnostic_microseconds,
                NULL, &provider_sample);
            profile_accumulate(&provider_profile, &provider_sample);
        } else {
            infer_status = mtfs_sentinel_npu_infer_detailed(&session->npu,
                input, npu_raw_output, npu_output, SENTINEL_TIMEOUT_MS,
                &npu_result);
        }
        end_us = mtfs_ra8p1_benchmark_clock_us(NULL);
        npu_total_us += end_us - start_us;
        if (infer_status != MTFS_OK) { stage = 18; ++failed; break; }
        ++completed;
        for (j = 0U; j < MTFS_SENTINEL_FEATURE_DIMENSION; ++j) {
            int delta = (int)cpu_output[j] - (int)npu_output[j];
            int raw_delta = (int)cpu_raw_output[j] - (int)npu_raw_output[j];
            uint32_t absolute = (uint32_t)(delta < 0 ? -delta : delta);
            uint32_t raw_absolute =
                (uint32_t)(raw_delta < 0 ? -raw_delta : raw_delta);
            if (absolute > max_error) {
                max_error = absolute; q4_error_index = j;
            }
            if (raw_absolute > max_raw_error) {
                max_raw_error = raw_absolute; raw_error_index = j;
            }
        }
        score_error = cpu_result.score_q8 > npu_result.score_q8 ?
            cpu_result.score_q8 - npu_result.score_q8 :
            npu_result.score_q8 - cpu_result.score_q8;
        if (mtfs_sentinel_score_interval_q8(input, cpu_output,
                SENTINEL_MAX_OUTPUT_ERROR_Q4, cpu_result.threshold_q8,
                &score_interval) != MTFS_OK) {
            stage = 19; ++failed; break;
        }
        if (max_raw_error > SENTINEL_MAX_RAW_OUTPUT_ERROR_INT8 ||
            max_error > SENTINEL_MAX_OUTPUT_ERROR_Q4 ||
            cpu_result.score_q8 != npu_result.score_q8 ||
            cpu_result.anomaly != npu_result.anomaly ||
            npu_result.score_q8 < score_interval.score_min_q8 ||
            npu_result.score_q8 > score_interval.score_max_q8) {
            stage = 20; ++failed; break;
        }
        if (score_interval.decision_class ==
                MTFS_SENTINEL_DECISION_AMBIGUOUS_CPU_ARBITRATION) {
            ++ambiguous; ++cpu_arbitrated;
        }
        ++accepted;
    }
    cleanup_failed = session_cleanup(session);
    target_diagnostics = session->ethosu.diagnostics;
    heap_after = mtfs_ra8p1_heap_call_count();
    mtfs_ra8p1_sentinel_rsip_memory_diagnostics(&rsip_capacity,
        &rsip_memory_verified);
    if (heap_after != heap_before) { stage = 21; ++failed; }
    if (!rsip_memory_verified) { stage = 24; ++failed; }
    if (cleanup_failed) ++failed;
    if (stage != 0 && failed == 0U) failed = 1U;
    if (stage == 20) {
        tm_printf((UB *)"[sentinel-infer] mismatch raw-index=%u cpu-raw=%d npu-raw=%d q4-index=%u cpu-q4=%d npu-q4=%d\n",
            raw_error_index, (int)cpu_raw_output[raw_error_index],
            (int)npu_raw_output[raw_error_index], q4_error_index,
            (int)cpu_output[q4_error_index], (int)npu_output[q4_error_index]);
    }
    tm_printf((UB *)"[sentinel-infer] %s attempted=%u completed=%u accepted=%u failed=%u cpu-score-q8=%u npu-score-q8=%u score-interval-q8=%u:%u decision=%u/%u ambiguous=%u cpu-arbitrated=%u max-raw-error-int8=%u max-output-error-q4=%u score-error-q8=%u cpu-us=%u npu-us=%u required-ram=%u heap-calls-delta=%u cleanup=%s\n",
        stage == 0 && !cleanup_failed ?
            (ambiguous != 0U ? (UB *)"PASS-CPU-ARBITRATED" : (UB *)"PASS") :
            (UB *)"FAIL", attempted, completed, accepted, failed,
        (UW)cpu_result.score_q8, (UW)npu_result.score_q8,
        (UW)score_interval.score_min_q8, (UW)score_interval.score_max_q8,
        cpu_result.anomaly, npu_result.anomaly, ambiguous, cpu_arbitrated,
        max_raw_error, max_error, (UW)score_error,
        average_u64(cpu_total_us, attempted),
        average_u64(npu_total_us, completed), required_ram,
        heap_after - heap_before,
        cleanup_failed ? (UB *)"FAIL" : (UB *)"PASS");
    tm_printf((UB *)"[sentinel-infer] rsip-memory fixed-capacity=%u guard=%s\n",
        rsip_capacity,
        rsip_memory_verified ? (UB *)"PASS" : (UB *)"FAIL");
    if (hotplug) {
        tm_printf((UB *)"[sentinel-infer-hotplug] pause=%s resume=%s warmup=%s resident-inference=%s resident-model=PASS reauthenticate-next-command=YES\n",
            media_paused ? (UB *)"PASS" : (UB *)"FAIL",
            media_resumed ? (UB *)"PASS" : (UB *)"FAIL",
            media_resumed && accepted != 0U ? (UB *)"PASS" : (UB *)"FAIL",
            media_resumed && stage == 0 && accepted == iterations ?
                (UB *)"PASS" : (UB *)"FAIL");
    }
    if (diagnostics) {
        uint64_t accounted = provider_profile.input_requantize_cycles +
            provider_profile.target_infer_cycles +
            provider_profile.output_requantize_cycles +
            provider_profile.score_decision_cycles;
        uint64_t other = provider_profile.total_cycles >= accounted ?
            provider_profile.total_cycles - accounted : 0U;
        mtfs_ra8p1_ethosu_hook_diagnostics_get(&hook_diagnostics);
        tm_printf((UB *)"[sentinel-profile] provider-avg-us samples=%u input-requant=%u target=%u output-requant=%u score-decision=%u other=%u total=%u\n",
            provider_profile.attempted,
            average_u64(provider_profile.input_requantize_cycles,
                provider_profile.attempted),
            average_u64(provider_profile.target_infer_cycles,
                provider_profile.attempted),
            average_u64(provider_profile.output_requantize_cycles,
                provider_profile.attempted),
            average_u64(provider_profile.score_decision_cycles,
                provider_profile.attempted),
            average_u64(other, provider_profile.attempted),
            average_u64(provider_profile.total_cycles,
                provider_profile.attempted));
        tm_printf((UB *)"[sentinel-profile] ethos-u installs=%u closes=%u invokes=%u invoke-failures=%u arena-used=%u model-bytes=%u cache-model-cleans=%u global-opens=%u global-open-failures=%u\n",
            target_diagnostics.installs, target_diagnostics.closes,
            target_diagnostics.invokes, target_diagnostics.invoke_failures,
            target_diagnostics.arena_used, target_diagnostics.model_bytes,
            target_diagnostics.cache_model_cleans,
            target_diagnostics.global_opens,
            target_diagnostics.global_open_failures);
        tm_printf((UB *)"[sentinel-profile] semaphore creates=%u destroys=%u active=%u peak=%u takes=%u gives=%u wfe=%u wait-us=%u timeouts=%u create-fail=%u rejected-destroy=%u rejected-give=%u\n",
            hook_diagnostics.creates, hook_diagnostics.destroys,
            hook_diagnostics.active, hook_diagnostics.peak_in_use,
            hook_diagnostics.takes, hook_diagnostics.gives,
            hook_diagnostics.wfe_calls, (UW)hook_diagnostics.wait_us,
            hook_diagnostics.timeouts, hook_diagnostics.create_failures,
            hook_diagnostics.rejected_destroys,
            hook_diagnostics.rejected_gives);
    }
    mtfs_secure_zero(input, sizeof(input));
    mtfs_secure_zero(raw, sizeof(raw));
    mtfs_secure_zero(cpu_output, sizeof(cpu_output));
    mtfs_secure_zero(npu_output, sizeof(npu_output));
    mtfs_secure_zero(cpu_raw_output, sizeof(cpu_raw_output));
    mtfs_secure_zero(npu_raw_output, sizeof(npu_raw_output));
    return stage == 0 && !cleanup_failed ? 0 : 1;
}

int mtfs_ra8p1_sentinel_inference_run(const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations)
{
    return inference_run(media, feature, NULL, iterations, 0, 0, NULL, NULL);
}

int mtfs_ra8p1_sentinel_inference_profile_run(
    const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations)
{
    return inference_run(media, feature, NULL, iterations, 0, 1, NULL, NULL);
}

int mtfs_ra8p1_sentinel_inference_hotplug_run(
    const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations,
    mtfs_ra8p1_sentinel_media_transition_fn transition,
    void *transition_context)
{
    return inference_run(media, feature, NULL, iterations, 1, 0,
        transition, transition_context);
}

int mtfs_ra8p1_sentinel_inference_vector_run(
    const mtfs_media_context_t *media, const int8_t input_q4[24],
    uint32_t iterations)
{
    return inference_run(media, NULL, input_q4, iterations, 0, 0, NULL, NULL);
}

mtfs_error_t mtfs_ra8p1_sentinel_monitor_open(void *media,
    uint64_t *threshold_q8)
{
    int stage;
    if (media == NULL || threshold_q8 == NULL)
        return MTFS_ERROR_INVALID_ARGUMENT;
    if (monitor_session.npu_open != 0U &&
        session_cleanup(&monitor_session) != 0)
        return MTFS_ERROR_INVALID_STATE;
    (void)memset(&monitor_session, 0, sizeof(monitor_session));
    monitor_session.media = media;
    stage = session_open(&monitor_session);
    if (stage != 0) {
        (void)session_cleanup(&monitor_session);
        tm_printf((UB *)"[sentinel-monitor] provider-open-stage=%d\n", stage);
        return stage == 3 || stage == 7 ? MTFS_ERROR_AUTHENTICATION :
            MTFS_ERROR_NOT_READY;
    }
    *threshold_q8 = monitor_session.bundle.threshold_q8;
    return MTFS_OK;
}

mtfs_error_t mtfs_ra8p1_sentinel_monitor_normalize(void *media,
    const mtfs_sentinel_feature_v1_t *feature, int8_t input_q4[24])
{
    uint32_t raw[MTFS_SENTINEL_FEATURE_DIMENSION];
    mtfs_error_t status;
    (void)media;
    if (monitor_session.npu_open == 0U || feature == NULL || input_q4 == NULL)
        return MTFS_ERROR_INVALID_STATE;
    status = mtfs_sentinel_feature_encode_raw(feature, raw);
    if (status == MTFS_OK)
        status = mtfs_sentinel_normalize_int8(
            &monitor_session.bundle.normalization, raw, input_q4);
    mtfs_secure_zero(raw, sizeof(raw));
    return status;
}

mtfs_error_t mtfs_ra8p1_sentinel_monitor_npu_infer(void *media,
    const int8_t input_q4[24], int8_t output_q4[24],
    mtfs_sentinel_inference_result_t *result, uint32_t *latency_us)
{
    uint64_t start_us, end_us;
    mtfs_error_t status;
    (void)media;
    if (monitor_session.npu_open == 0U || input_q4 == NULL ||
        output_q4 == NULL || result == NULL || latency_us == NULL)
        return MTFS_ERROR_INVALID_STATE;
    start_us = mtfs_ra8p1_benchmark_clock_us(NULL);
    status = mtfs_sentinel_npu_infer(&monitor_session.npu, input_q4,
        output_q4, SENTINEL_TIMEOUT_MS, result);
    end_us = mtfs_ra8p1_benchmark_clock_us(NULL);
    *latency_us = end_us >= start_us && end_us - start_us <= UINT32_MAX ?
        (uint32_t)(end_us - start_us) : UINT32_MAX;
    return status;
}

mtfs_error_t mtfs_ra8p1_sentinel_monitor_cpu_infer(void *media,
    const int8_t input_q4[24], mtfs_sentinel_inference_result_t *result,
    uint32_t *latency_us)
{
    int8_t output_q4[MTFS_SENTINEL_FEATURE_DIMENSION];
    uint64_t start_us, end_us;
    mtfs_error_t status;
    (void)media;
    if (monitor_session.npu_open == 0U || input_q4 == NULL ||
        result == NULL || latency_us == NULL)
        return MTFS_ERROR_INVALID_STATE;
    start_us = mtfs_ra8p1_benchmark_clock_us(NULL);
    status = mtfs_sentinel_cpu_infer(&monitor_session.cpu, input_q4,
        arena + monitor_session.plan.scratch_offset,
        monitor_session.plan.scratch_size, output_q4, result);
    end_us = mtfs_ra8p1_benchmark_clock_us(NULL);
    *latency_us = end_us >= start_us && end_us - start_us <= UINT32_MAX ?
        (uint32_t)(end_us - start_us) : UINT32_MAX;
    mtfs_secure_zero(output_q4, sizeof(output_q4));
    return status;
}

mtfs_error_t mtfs_ra8p1_sentinel_monitor_close(void *media)
{
    (void)media;
    return session_cleanup(&monitor_session) == 0 ? MTFS_OK : MTFS_ERROR_IO;
}

#else

int mtfs_ra8p1_sentinel_inference_run(const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations)
{
    (void)media; (void)feature; (void)iterations; return 1;
}
int mtfs_ra8p1_sentinel_inference_profile_run(
    const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations)
{
    (void)media; (void)feature; (void)iterations; return 1;
}
int mtfs_ra8p1_sentinel_inference_hotplug_run(
    const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations,
    mtfs_ra8p1_sentinel_media_transition_fn transition,
    void *transition_context)
{
    (void)media; (void)feature; (void)iterations; (void)transition;
    (void)transition_context; return 1;
}
int mtfs_ra8p1_sentinel_inference_vector_run(
    const mtfs_media_context_t *media, const int8_t input_q4[24],
    uint32_t iterations)
{
    (void)media; (void)input_q4; (void)iterations; return 1;
}

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE
mtfs_error_t mtfs_ra8p1_sentinel_monitor_open(void *media,
    uint64_t *threshold_q8)
{
    (void)media; (void)threshold_q8; return MTFS_ERROR_NOT_SUPPORTED;
}
mtfs_error_t mtfs_ra8p1_sentinel_monitor_normalize(void *media,
    const mtfs_sentinel_feature_v1_t *feature, int8_t input_q4[24])
{
    (void)media; (void)feature; (void)input_q4;
    return MTFS_ERROR_NOT_SUPPORTED;
}
mtfs_error_t mtfs_ra8p1_sentinel_monitor_npu_infer(void *media,
    const int8_t input_q4[24], int8_t output_q4[24],
    mtfs_sentinel_inference_result_t *result, uint32_t *latency_us)
{
    (void)media; (void)input_q4; (void)output_q4; (void)result;
    (void)latency_us; return MTFS_ERROR_NOT_SUPPORTED;
}
mtfs_error_t mtfs_ra8p1_sentinel_monitor_cpu_infer(void *media,
    const int8_t input_q4[24], mtfs_sentinel_inference_result_t *result,
    uint32_t *latency_us)
{
    (void)media; (void)input_q4; (void)result; (void)latency_us;
    return MTFS_ERROR_NOT_SUPPORTED;
}
mtfs_error_t mtfs_ra8p1_sentinel_monitor_close(void *media)
{
    (void)media; return MTFS_ERROR_NOT_SUPPORTED;
}
#endif

#endif
