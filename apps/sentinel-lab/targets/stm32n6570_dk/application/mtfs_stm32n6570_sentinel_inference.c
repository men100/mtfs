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
#define SENTINEL_PROFILE_ID (UINT32_C(0x9830ca59))
#define SENTINEL_ARENA_SIZE (UINT32_C(32768))
#define SENTINEL_TIMEOUT_MS (UINT32_C(2000))
#define SENTINEL_HOTPLUG_WAIT_TICKS (UINT32_C(12000))
#define SENTINEL_MAX_OUTPUT_ERROR_Q4 (1)
#define SENTINEL_MAX_RAW_OUTPUT_ERROR_INT8 (1)
#define SENTINEL_SCHEDULER_PROBE_STACK_SIZE (UINT32_C(1024))
#define SENTINEL_SCHEDULER_PROBE_PRIORITY (32)

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
static uint8_t scheduler_probe_stack[SENTINEL_SCHEDULER_PROBE_STACK_SIZE]
    SENTINEL_ALIGN32;
static volatile uint32_t scheduler_probe_count;
static sentinel_session_t monitor_session;

static void scheduler_probe_task(INT start_code, void *context)
{
    (void)start_code;
    (void)context;
    for (;;) {
        if (scheduler_probe_count != UINT32_MAX) ++scheduler_probe_count;
    }
}

static int scheduler_probe_start(ID *task_id)
{
    T_CTSK task = {0};
    ER status;
    if (task_id == NULL) return 1;
    scheduler_probe_count = 0U;
    task.tskatr = TA_HLNG | TA_RNG3 | TA_USERBUF;
    task.task = scheduler_probe_task;
    task.itskpri = SENTINEL_SCHEDULER_PROBE_PRIORITY;
    task.stksz = sizeof(scheduler_probe_stack);
    task.bufptr = scheduler_probe_stack;
    *task_id = tk_cre_tsk(&task);
    if (*task_id <= 0) return 1;
    status = tk_sta_tsk(*task_id, 0);
    if (status < E_OK) {
        (void)tk_del_tsk(*task_id);
        *task_id = 0;
        return 1;
    }
    return 0;
}

static int scheduler_probe_stop(ID task_id, uint32_t *count)
{
    ER terminate_status, delete_status;
    if (task_id <= 0 || count == NULL) return 1;
    terminate_status = tk_ter_tsk(task_id);
    *count = scheduler_probe_count;
    delete_status = tk_del_tsk(task_id);
    return terminate_status < E_OK || delete_status < E_OK;
}

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

static uint32_t diagnostic_cycle_count(void *opaque)
{
    (void)opaque;
    return mtfs_stm32n6570_dk_cycle_count();
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
    /* The authenticated payload is resident; release the SD/FatFs handle. */
    status = mtfs_sealed_reader_fatfs_close(&session->fatfs_reader);
    if (status != MTFS_OK) return 7;
    session->reader_open = 0U;
    (void)memset(&session->reader, 0, sizeof(session->reader));
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

static uint32_t average_cycles_to_us(uint64_t total, uint32_t count,
    uint32_t clock_hz)
{
    uint64_t average, scaled;
    if (count == 0U || clock_hz == 0U) return 0U;
    average = total / count;
    scaled = average * UINT64_C(1000000);
    return (uint32_t)((scaled + clock_hz - 1U) / clock_hz);
}

static uint32_t average_cycles(uint64_t total, uint32_t count)
{
    return count == 0U ? 0U : (uint32_t)(total / count);
}

static uint64_t residual_cycles(uint64_t total, uint64_t accounted)
{
    return total >= accounted ? total - accounted : 0U;
}

static void profile_accumulate(
    mtfs_sentinel_npu_inference_profile_t *total,
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

static int inference_run(
    const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, const int8_t supplied_input[24],
    uint32_t iterations, int hotplug, int diagnostics)
{
    static sentinel_session_t session;
    uint32_t raw[MTFS_SENTINEL_FEATURE_DIMENSION];
    int8_t input[MTFS_SENTINEL_FEATURE_DIMENSION];
    int8_t cpu_output[MTFS_SENTINEL_FEATURE_DIMENSION];
    int8_t npu_output[MTFS_SENTINEL_FEATURE_DIMENSION];
    int8_t cpu_raw_output[MTFS_SENTINEL_FEATURE_DIMENSION];
    int8_t npu_raw_output[MTFS_SENTINEL_FEATURE_DIMENSION];
    mtfs_sentinel_inference_result_t cpu_result, npu_result;
    mtfs_sentinel_score_interval_t score_interval;
    mtfs_sentinel_npu_inference_profile_t provider_profile;
    mtfs_sentinel_npu_inference_profile_t provider_sample;
    mtfs_stm32n6_neural_art_profile_t neural_art_profile;
    uint64_t cpu_total_cycles = 0U, npu_total_cycles = 0U;
    uint64_t score_error = 0U;
    uint32_t i, j, start_cycles, elapsed_cycles, max_error = 0U;
    uint32_t max_raw_error = 0U, attempted = 0U, completed = 0U;
    uint32_t failed = 0U, accepted = 0U, cpu_completed = 0U, npu_completed = 0U;
    uint32_t raw_error_index = 0U, q4_error_index = 0U;
    uint32_t ambiguous = 0U, cpu_arbitrated = 0U;
    uint32_t cycle_clock_hz = mtfs_stm32n6570_dk_cycle_clock_hz();
    uint32_t required_ram = 0U;
    uint32_t scheduler_count = 0U;
    ID scheduler_task_id = 0;
    mtfs_error_t infer_status;
    int stage, cleanup_failed, reopen_attempted = 0, reopen_passed = 0;
    int scheduler_started = 0, neural_profile_started = 0;

    if (media == NULL || (feature == NULL && supplied_input == NULL) ||
        (feature != NULL && supplied_input != NULL) || iterations == 0U) return 1;
    if (session.npu_open != 0U && session_cleanup(&session) != 0) {
        tm_printf((UB *)"[sentinel-infer] FAIL prior NPU cleanup still pending\n");
        return 1;
    }
    (void)memset(&session, 0, sizeof(session));
    (void)memset(&cpu_result, 0, sizeof(cpu_result));
    (void)memset(&npu_result, 0, sizeof(npu_result));
    (void)memset(&score_interval, 0, sizeof(score_interval));
    (void)memset(&provider_profile, 0, sizeof(provider_profile));
    (void)memset(&neural_art_profile, 0, sizeof(neural_art_profile));
    session.media = media;
    stage = session_open(&session);
    if (stage != 0) {
        cleanup_failed = session_cleanup(&session);
        tm_printf((UB *)"[sentinel-infer] FAIL open-stage=%d cleanup=%s\n",
            stage, cleanup_failed ? (UB *)"FAIL" : (UB *)"PASS");
        return 1;
    }
    required_ram = session.plan.required_ram;
    if (hotplug) {
        tm_printf((UB *)"[sentinel-infer-hotplug] ACTION REQUIRED: REMOVE card; resident CPU/NPU inference will continue\n");
        if (wait_media(media, 0) != 0) stage = 19;
    }
    if (supplied_input != NULL) {
        (void)memcpy(input, supplied_input, sizeof(input));
    } else if (mtfs_sentinel_feature_encode_raw(feature, raw) != MTFS_OK ||
               mtfs_sentinel_normalize_int8(&session.bundle.normalization, raw,
                   input) != MTFS_OK) {
        stage = 15;
    }
    if (stage == 0 && cycle_clock_hz == 0U) stage = 21;
    if (stage == 0 && diagnostics) {
        if (mtfs_stm32n6_neural_art_profile_start(&session.neural_art,
                diagnostic_cycle_count, NULL) != MTFS_OK) {
            stage = 24;
        } else {
            neural_profile_started = 1;
            if (scheduler_probe_start(&scheduler_task_id) != 0) stage = 25;
            else scheduler_started = 1;
        }
    }
    for (i = 0U; stage == 0 && i < iterations; ++i) {
        ++attempted;
        start_cycles = mtfs_stm32n6570_dk_cycle_count();
        if (mtfs_sentinel_cpu_infer_detailed(&session.cpu, input,
                arena + session.plan.scratch_offset, session.plan.scratch_size,
                cpu_raw_output, cpu_output, &cpu_result) != MTFS_OK) {
            stage = 16; ++failed; break;
        }
        elapsed_cycles = mtfs_stm32n6570_dk_cycle_count() - start_cycles;
        cpu_total_cycles += elapsed_cycles; ++cpu_completed;
        start_cycles = mtfs_stm32n6570_dk_cycle_count();
        if (diagnostics) {
            infer_status = mtfs_sentinel_npu_infer_profiled_detailed(
                &session.npu, input, npu_raw_output, npu_output,
                SENTINEL_TIMEOUT_MS, &npu_result,
                diagnostic_cycle_count, NULL, &provider_sample);
            profile_accumulate(&provider_profile, &provider_sample);
        } else {
            infer_status = mtfs_sentinel_npu_infer_detailed(&session.npu,
                input, npu_raw_output, npu_output, SENTINEL_TIMEOUT_MS,
                &npu_result);
        }
        if (infer_status != MTFS_OK) { stage = 17; ++failed; break; }
        elapsed_cycles = mtfs_stm32n6570_dk_cycle_count() - start_cycles;
        npu_total_cycles += elapsed_cycles; ++npu_completed; ++completed;
        for (j = 0U; j < MTFS_SENTINEL_FEATURE_DIMENSION; ++j) {
            int delta = (int)cpu_output[j] - (int)npu_output[j];
            uint32_t absolute = (uint32_t)(delta < 0 ? -delta : delta);
            int raw_delta = (int)cpu_raw_output[j] - (int)npu_raw_output[j];
            uint32_t raw_absolute = (uint32_t)(raw_delta < 0 ? -raw_delta : raw_delta);
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
            stage = 23; ++failed; break;
        }
        if (max_raw_error > SENTINEL_MAX_RAW_OUTPUT_ERROR_INT8 ||
            max_error > SENTINEL_MAX_OUTPUT_ERROR_Q4 ||
            cpu_result.score_q8 < score_interval.score_min_q8 ||
            cpu_result.score_q8 > score_interval.score_max_q8 ||
            npu_result.score_q8 < score_interval.score_min_q8 ||
            npu_result.score_q8 > score_interval.score_max_q8 ||
            (score_interval.decision_class ==
                MTFS_SENTINEL_DECISION_DEFINITELY_NORMAL &&
                npu_result.anomaly != 0U) ||
            (score_interval.decision_class ==
                MTFS_SENTINEL_DECISION_DEFINITELY_ANOMALY &&
                npu_result.anomaly == 0U)) {
            stage = 18; ++failed; break;
        }
        if (score_interval.decision_class ==
                MTFS_SENTINEL_DECISION_AMBIGUOUS_CPU_ARBITRATION) {
            ++ambiguous;
            ++cpu_arbitrated;
        }
        ++accepted;
    }
    if (scheduler_started &&
        scheduler_probe_stop(scheduler_task_id, &scheduler_count) != 0) {
        if (stage == 0) stage = 26;
    }
    if (neural_profile_started &&
        mtfs_stm32n6_neural_art_profile_stop(&session.neural_art,
            &neural_art_profile) != MTFS_OK) {
        if (stage == 0) stage = 27;
    }
    cleanup_failed = session_cleanup(&session);
    if (hotplug && cleanup_failed == 0) {
        tm_printf((UB *)"[sentinel-infer-hotplug] ACTION REQUIRED: REINSERT card\n");
        if (wait_media(media, 1) != 0) {
            if (stage == 0) stage = 20;
        } else {
            reopen_attempted = 1;
            (void)memset(&session, 0, sizeof(session));
            session.media = media;
            if (session_open(&session) != 0) {
                if (stage == 0) stage = 22;
            } else {
                reopen_passed = 1;
            }
            if (session_cleanup(&session) != 0) cleanup_failed = 1;
        }
    }
    if (stage != 0 && failed == 0U) failed = 1U;
    if (cleanup_failed) ++failed;
    if (stage == 18) {
        tm_printf((UB *)"[sentinel-infer] mismatch raw-index=%u cpu-raw=%d npu-raw=%d q4-index=%u cpu-q4=%d npu-q4=%d\n",
            raw_error_index, (int)cpu_raw_output[raw_error_index],
            (int)npu_raw_output[raw_error_index], q4_error_index,
            (int)cpu_output[q4_error_index], (int)npu_output[q4_error_index]);
    }
    tm_printf((UB *)"[sentinel-infer] %s attempted=%u completed=%u accepted=%u failed=%u cpu-score-q8=%u npu-score-q8=%u score-interval-q8=%u:%u decision=%u/%u ambiguous=%u cpu-arbitrated=%u max-raw-error-int8=%u max-output-error-q4=%u score-error-q8=%u cpu-cycles=%u npu-cycles=%u cpu-us=%u npu-us=%u clock-hz=%u required-ram=%u cleanup=%s\n",
        stage == 0 && !cleanup_failed ?
            (ambiguous != 0U ? (UB *)"PASS-CPU-ARBITRATED" : (UB *)"PASS") :
            (UB *)"FAIL",
        attempted, completed, accepted, failed,
        (UW)cpu_result.score_q8, (UW)npu_result.score_q8,
        (UW)score_interval.score_min_q8, (UW)score_interval.score_max_q8,
        cpu_result.anomaly, npu_result.anomaly, ambiguous, cpu_arbitrated,
        max_raw_error, max_error,
        (UW)score_error,
        cpu_completed == 0U ? 0U : (UW)(cpu_total_cycles / cpu_completed),
        npu_completed == 0U ? 0U : (UW)(npu_total_cycles / npu_completed),
        average_cycles_to_us(cpu_total_cycles, cpu_completed, cycle_clock_hz),
        average_cycles_to_us(npu_total_cycles, npu_completed, cycle_clock_hz),
        cycle_clock_hz,
        (UW)required_ram,
        cleanup_failed ? (UB *)"FAIL" : (UB *)"PASS");
    if (hotplug) {
        tm_printf((UB *)"[sentinel-infer-hotplug] reopen=%s\n",
            reopen_attempted && reopen_passed && !cleanup_failed ?
                (UB *)"PASS" : (UB *)"FAIL");
    }
    if (diagnostics) {
        uint64_t provider_accounted =
            provider_profile.input_requantize_cycles +
            provider_profile.target_infer_cycles +
            provider_profile.output_requantize_cycles +
            provider_profile.score_decision_cycles;
        uint64_t neural_accounted = neural_art_profile.input_copy_cycles +
            neural_art_profile.cache_clean_cycles +
            neural_art_profile.reset_cycles + neural_art_profile.epoch_cycles +
            neural_art_profile.wait_cycles +
            neural_art_profile.cache_invalidate_cycles +
            neural_art_profile.output_copy_cycles;
        tm_printf((UB *)"[sentinel-profile] provider-avg-cycles samples=%u input-requant=%u target=%u output-requant=%u score-decision=%u other=%u total=%u total-us=%u\n",
            provider_profile.attempted,
            average_cycles(provider_profile.input_requantize_cycles,
                provider_profile.attempted),
            average_cycles(provider_profile.target_infer_cycles,
                provider_profile.attempted),
            average_cycles(provider_profile.output_requantize_cycles,
                provider_profile.attempted),
            average_cycles(provider_profile.score_decision_cycles,
                provider_profile.attempted),
            average_cycles(residual_cycles(provider_profile.total_cycles,
                provider_accounted), provider_profile.attempted),
            average_cycles(provider_profile.total_cycles,
                provider_profile.attempted),
            average_cycles_to_us(provider_profile.total_cycles,
                provider_profile.attempted, cycle_clock_hz));
        tm_printf((UB *)"[sentinel-profile] neural-art-avg-cycles samples=%u input-copy=%u cache-clean=%u reset=%u epoch-call=%u wait=%u cache-invalidate=%u output-copy=%u other=%u total=%u\n",
            neural_art_profile.attempted,
            average_cycles(neural_art_profile.input_copy_cycles,
                neural_art_profile.attempted),
            average_cycles(neural_art_profile.cache_clean_cycles,
                neural_art_profile.attempted),
            average_cycles(neural_art_profile.reset_cycles,
                neural_art_profile.attempted),
            average_cycles(neural_art_profile.epoch_cycles,
                neural_art_profile.attempted),
            average_cycles(neural_art_profile.wait_cycles,
                neural_art_profile.attempted),
            average_cycles(neural_art_profile.cache_invalidate_cycles,
                neural_art_profile.attempted),
            average_cycles(neural_art_profile.output_copy_cycles,
                neural_art_profile.attempted),
            average_cycles(residual_cycles(neural_art_profile.total_cycles,
                neural_accounted), neural_art_profile.attempted),
            average_cycles(neural_art_profile.total_cycles,
                neural_art_profile.attempted));
        tm_printf((UB *)"[sentinel-profile] neural-art-avg-us samples=%u input-copy=%u cache-clean=%u reset=%u epoch-call=%u event-wait=%u cache-invalidate=%u output-copy=%u other=%u total=%u\n",
            neural_art_profile.attempted,
            average_cycles_to_us(neural_art_profile.input_copy_cycles,
                neural_art_profile.attempted, cycle_clock_hz),
            average_cycles_to_us(neural_art_profile.cache_clean_cycles,
                neural_art_profile.attempted, cycle_clock_hz),
            average_cycles_to_us(neural_art_profile.reset_cycles,
                neural_art_profile.attempted, cycle_clock_hz),
            average_cycles_to_us(neural_art_profile.epoch_cycles,
                neural_art_profile.attempted, cycle_clock_hz),
            average_cycles_to_us(neural_art_profile.wait_cycles,
                neural_art_profile.attempted, cycle_clock_hz),
            average_cycles_to_us(neural_art_profile.cache_invalidate_cycles,
                neural_art_profile.attempted, cycle_clock_hz),
            average_cycles_to_us(neural_art_profile.output_copy_cycles,
                neural_art_profile.attempted, cycle_clock_hz),
            average_cycles_to_us(residual_cycles(
                neural_art_profile.total_cycles, neural_accounted),
                neural_art_profile.attempted, cycle_clock_hz),
            average_cycles_to_us(neural_art_profile.total_cycles,
                neural_art_profile.attempted, cycle_clock_hz));
        tm_printf((UB *)"[sentinel-profile] neural-art-states attempted=%u completed=%u failed=%u epoch-calls=%u no-wfe=%u wfe=%u done=%u other=%u wait-starts=%u\n",
            neural_art_profile.attempted, neural_art_profile.completed,
            neural_art_profile.failed, neural_art_profile.epoch_calls,
            neural_art_profile.state_no_wfe, neural_art_profile.state_wfe,
            neural_art_profile.state_done, neural_art_profile.state_other,
            neural_art_profile.event_wait_starts);
        tm_printf((UB *)"[sentinel-profile] neural-art-events irq=%u immediate=%u spurious=%u late=%u timeout=%u kernel-error=%u last-kernel-error=%d no-wfe-max=%u no-wfe-limit=%u recoveries=%u scheduler-probe-count=%u scheduler-probe=%s\n",
            neural_art_profile.event_irq_notifications,
            neural_art_profile.immediate_event_completions,
            neural_art_profile.spurious_notifications,
            neural_art_profile.late_notifications,
            neural_art_profile.timeouts,
            neural_art_profile.kernel_wait_errors,
            neural_art_profile.last_kernel_error,
            neural_art_profile.consecutive_no_wfe_max,
            neural_art_profile.consecutive_no_wfe_limit_errors,
            neural_art_profile.recoveries, scheduler_count,
            scheduler_started && scheduler_count != 0U ?
                (UB *)"OBSERVED" : (UB *)"NOT-OBSERVED");
    }
    return stage == 0 && !cleanup_failed ? 0 : 1;
}

int mtfs_stm32n6570_sentinel_inference_run(
    const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations)
{
    return inference_run(media, feature, NULL, iterations, 0, 0);
}

int mtfs_stm32n6570_sentinel_inference_hotplug_run(
    const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations)
{
    return inference_run(media, feature, NULL, iterations, 1, 0);
}

int mtfs_stm32n6570_sentinel_inference_profile_run(
    const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations)
{
    return inference_run(media, feature, NULL, iterations, 0, 1);
}

int mtfs_stm32n6570_sentinel_inference_vector_run(
    const mtfs_media_context_t *media, const int8_t input_q4[24],
    uint32_t iterations)
{
    return inference_run(media, NULL, input_q4, iterations, 0, 0);
}

mtfs_error_t mtfs_stm32n6570_sentinel_monitor_open(void *media,
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
        return stage == 5 || stage == 8 ? MTFS_ERROR_AUTHENTICATION :
            MTFS_ERROR_NOT_READY;
    }
    *threshold_q8 = monitor_session.bundle.threshold_q8;
    return MTFS_OK;
}

mtfs_error_t mtfs_stm32n6570_sentinel_monitor_normalize(void *media,
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

mtfs_error_t mtfs_stm32n6570_sentinel_monitor_npu_infer(void *media,
    const int8_t input_q4[24], int8_t output_q4[24],
    mtfs_sentinel_inference_result_t *result, uint32_t *latency_us)
{
    uint64_t start_us;
    uint64_t end_us;
    mtfs_error_t status;
    (void)media;
    if (monitor_session.npu_open == 0U || input_q4 == NULL ||
        output_q4 == NULL || result == NULL || latency_us == NULL)
        return MTFS_ERROR_INVALID_STATE;
    start_us = mtfs_stm32n6570_dk_benchmark_clock_us(NULL);
    status = mtfs_sentinel_npu_infer(&monitor_session.npu, input_q4,
        output_q4, SENTINEL_TIMEOUT_MS, result);
    end_us = mtfs_stm32n6570_dk_benchmark_clock_us(NULL);
    *latency_us = end_us >= start_us && end_us - start_us <= UINT32_MAX ?
        (uint32_t)(end_us - start_us) : UINT32_MAX;
    return status;
}

mtfs_error_t mtfs_stm32n6570_sentinel_monitor_cpu_infer(void *media,
    const int8_t input_q4[24], mtfs_sentinel_inference_result_t *result,
    uint32_t *latency_us)
{
    int8_t output_q4[MTFS_SENTINEL_FEATURE_DIMENSION];
    uint64_t start_us;
    uint64_t end_us;
    mtfs_error_t status;
    (void)media;
    if (monitor_session.npu_open == 0U || input_q4 == NULL ||
        result == NULL || latency_us == NULL)
        return MTFS_ERROR_INVALID_STATE;
    start_us = mtfs_stm32n6570_dk_benchmark_clock_us(NULL);
    status = mtfs_sentinel_cpu_infer(&monitor_session.cpu, input_q4,
        arena + monitor_session.plan.scratch_offset,
        monitor_session.plan.scratch_size, output_q4, result);
    end_us = mtfs_stm32n6570_dk_benchmark_clock_us(NULL);
    *latency_us = end_us >= start_us && end_us - start_us <= UINT32_MAX ?
        (uint32_t)(end_us - start_us) : UINT32_MAX;
    mtfs_secure_zero(output_q4, sizeof(output_q4));
    return status;
}

mtfs_error_t mtfs_stm32n6570_sentinel_monitor_close(void *media)
{
    (void)media;
    return session_cleanup(&monitor_session) == 0 ? MTFS_OK : MTFS_ERROR_IO;
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

int mtfs_stm32n6570_sentinel_inference_profile_run(
    const mtfs_media_context_t *media,
    const mtfs_sentinel_feature_v1_t *feature, uint32_t iterations)
{
    (void)media; (void)feature; (void)iterations;
    return 1;
}

int mtfs_stm32n6570_sentinel_inference_vector_run(
    const mtfs_media_context_t *media, const int8_t input_q4[24],
    uint32_t iterations)
{
    (void)media; (void)input_q4; (void)iterations;
    return 1;
}

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE
mtfs_error_t mtfs_stm32n6570_sentinel_monitor_open(void *media,
    uint64_t *threshold_q8)
{
    (void)media; (void)threshold_q8; return MTFS_ERROR_NOT_SUPPORTED;
}
mtfs_error_t mtfs_stm32n6570_sentinel_monitor_normalize(void *media,
    const mtfs_sentinel_feature_v1_t *feature, int8_t input_q4[24])
{
    (void)media; (void)feature; (void)input_q4;
    return MTFS_ERROR_NOT_SUPPORTED;
}
mtfs_error_t mtfs_stm32n6570_sentinel_monitor_npu_infer(void *media,
    const int8_t input_q4[24], int8_t output_q4[24],
    mtfs_sentinel_inference_result_t *result, uint32_t *latency_us)
{
    (void)media; (void)input_q4; (void)output_q4; (void)result;
    (void)latency_us; return MTFS_ERROR_NOT_SUPPORTED;
}
mtfs_error_t mtfs_stm32n6570_sentinel_monitor_cpu_infer(void *media,
    const int8_t input_q4[24], mtfs_sentinel_inference_result_t *result,
    uint32_t *latency_us)
{
    (void)media; (void)input_q4; (void)result; (void)latency_us;
    return MTFS_ERROR_NOT_SUPPORTED;
}
mtfs_error_t mtfs_stm32n6570_sentinel_monitor_close(void *media)
{
    (void)media; return MTFS_OK;
}
#endif

#endif
