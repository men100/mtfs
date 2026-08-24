#include "mtfs_stm32n6570_model_test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "ff.h"
#include "diskio.h"
#include "mtfs_model_store.h"
#include "mtfs_sealed_format.h"
#include "mtfs_sealed_reader_fatfs.h"
#include "mtfs_secure_zero.h"
#include "mtfs_stm32_saes_provider.h"
#include "mtfs_stm32n6570_nor.h"
#include "mtfs_stm32n6570_crypto_work.h"
#include "../../../../common/mtfs_model_test_registry.h"

#define MODEL_PATH "0:/MTFSTEST.MTF"
#define MODEL_PAYLOAD_BYTES (5000U)
#define MODEL_EXTRA_BYTES (17U)
#define HOTPLUG_WAIT_TICKS (12000U)

#if defined(__GNUC__)
#define MODEL_ALIGN __attribute__((aligned(32)))
#else
#define MODEL_ALIGN
#endif

static uint8_t manifest_work[MTFS_SEALED_MAX_MANIFEST_SIZE] MODEL_ALIGN;
static uint8_t aad_work[MTFS_SEALED_CHUNK_AAD_SIZE] MODEL_ALIGN;
static uint8_t destination[MODEL_PAYLOAD_BYTES + MODEL_EXTRA_BYTES] MODEL_ALIGN;

#define ciphertext_work mtfs_stm32n6570_test_ciphertext_work
#define plaintext_work mtfs_stm32n6570_test_plaintext_work

typedef struct model_reader_decorator
{
    mtfs_sealed_reader_t base;
    uint64_t mutation_offset;
    uint64_t payload_offset;
    uint32_t payload_reads;
    uint8_t mutate;
} model_reader_decorator_t;

typedef struct model_session
{
    FATFS filesystem;
    mtfs_sealed_reader_fatfs_t fatfs;
    mtfs_sealed_reader_t reader;
    model_reader_decorator_t decorator;
    mtfs_sealed_reader_t decorated_reader;
    mtfs_stm32_saes_provider_context_t provider_context;
    mtfs_crypto_provider_t provider;
    mtfs_stm32_nor_io_t nor;
    mtfs_model_t model;
    ID mutex_id;
    const mtfs_media_context_t *media;
    uint8_t mounted;
    uint8_t reader_open;
    uint8_t provider_open;
    uint8_t model_initialized;
} model_session_t;

static mtfs_sealed_work_t model_work(void)
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
    memset(&policy, 0, sizeof(policy));
    policy.api_version = MTFS_MODEL_POLICY_API_VERSION;
    policy.struct_size = (uint32_t)sizeof(policy);
    policy.expected_target_id = MTFS_MODEL_TEST_TARGET_ID;
    policy.expected_accelerator_id = MTFS_MODEL_TEST_ACCELERATOR_ID;
    policy.accepted_model_format = MTFS_MODEL_TEST_FORMAT_ID;
    policy.maximum_chunk_size = MTFS_SEALED_MAX_CHUNK_SIZE;
    policy.maximum_model_payload_size = MODEL_PAYLOAD_BYTES;
    policy.maximum_required_ram = MODEL_PAYLOAD_BYTES;
    return policy;
}

static int all_value(const uint8_t *data, size_t bytes, uint8_t value)
{
    size_t index;
    for (index = 0U; index < bytes; ++index)
        if (data[index] != value)
            return 0;
    return 1;
}

static int known_payload(const uint8_t *data)
{
    size_t index;
    for (index = 0U; index < MODEL_PAYLOAD_BYTES; ++index)
        if (data[index] != (uint8_t)((index * 7U + 3U) & 0xffU))
            return 0;
    return 1;
}

static int work_is_zero(void)
{
    return all_value(manifest_work, sizeof(manifest_work), 0U) &&
        all_value(aad_work, sizeof(aad_work), 0U) &&
        all_value(ciphertext_work, sizeof(ciphertext_work), 0U) &&
        all_value(plaintext_work, sizeof(plaintext_work), 0U);
}

static mtfs_error_t target_media_status(void *opaque)
{
    const mtfs_media_context_t *media =
        (const mtfs_media_context_t *)opaque;
    mtfs_media_state_t state;
    if (media == NULL)
        return MTFS_ERROR_NOT_READY;
    state = mtfs_media_state(media);
    if (state == MTFS_MEDIA_STATE_PRESENT)
        return MTFS_OK;
    if (state == MTFS_MEDIA_STATE_ABSENT)
        return MTFS_ERROR_NO_MEDIA;
    return MTFS_ERROR_NOT_READY;
}

static int provider_lock(void *opaque)
{
    ID mutex_id = *(ID *)opaque;
    return tk_loc_mtx(mutex_id, TMO_FEVR) == E_OK ? 0 : -1;
}

static void provider_unlock(void *opaque)
{
    ID mutex_id = *(ID *)opaque;
    (void)tk_unl_mtx(mutex_id);
}

static mtfs_error_t decorated_get_size(void *opaque, uint64_t *size)
{
    model_reader_decorator_t *decorator =
        (model_reader_decorator_t *)opaque;
    return decorator->base.get_size(decorator->base.context, size);
}

static mtfs_error_t decorated_read_at(void *opaque, uint64_t offset,
    void *buffer, size_t requested, size_t *read_size)
{
    model_reader_decorator_t *decorator =
        (model_reader_decorator_t *)opaque;
    mtfs_error_t result = decorator->base.read_at(decorator->base.context,
        offset, buffer, requested, read_size);
    if (offset >= decorator->payload_offset)
        ++decorator->payload_reads;
    if (result == MTFS_OK && decorator->mutate != 0U &&
        decorator->mutation_offset >= offset &&
        decorator->mutation_offset - offset < requested) {
        ((uint8_t *)buffer)[(size_t)(decorator->mutation_offset - offset)] ^= 1U;
    }
    return result;
}

static void decorate(model_session_t *session, uint64_t mutation,
    uint64_t payload_offset, int mutate)
{
    memset(&session->decorator, 0, sizeof(session->decorator));
    session->decorator.base = session->reader;
    session->decorator.mutation_offset = mutation;
    session->decorator.payload_offset = payload_offset;
    session->decorator.mutate = mutate ? 1U : 0U;
    session->decorated_reader.api_version = MTFS_SEALED_READER_API_VERSION;
    session->decorated_reader.struct_size =
        (uint32_t)sizeof(session->decorated_reader);
    session->decorated_reader.context = &session->decorator;
    session->decorated_reader.get_size = decorated_get_size;
    session->decorated_reader.read_at = decorated_read_at;
}

static int session_setup(model_session_t *session,
    const mtfs_media_context_t *media)
{
    T_CMTX mutex = {.mtxatr = TA_INHERIT};
    int32_t bsp_error = 0;
    memset(session, 0, sizeof(*session));
    session->media = media;
    mtfs_model_init(&session->model);
    session->model_initialized = 1U;
    mtfs_sealed_reader_fatfs_init(&session->fatfs);
    if (f_mount(&session->filesystem, "0:", 1U) != FR_OK)
        return -1;
    session->mounted = 1U;
    if (mtfs_sealed_reader_fatfs_open(&session->fatfs, MODEL_PATH,
            target_media_status, (void *)media, &session->reader) != MTFS_OK)
        return -2;
    session->reader_open = 1U;
    session->mutex_id = tk_cre_mtx(&mutex);
    if (session->mutex_id <= 0)
        return -3;
    if (mtfs_stm32n6570_nor_open(&session->nor, &bsp_error) != 0)
        return -4;
    if (mtfs_stm32_saes_provider_init(&session->provider_context,
            &session->nor, provider_lock, provider_unlock,
            &session->mutex_id, &session->provider) != MTFS_CRYPTO_OK)
        return -5;
    session->provider_open = 1U;
    return 0;
}

static int session_cleanup(model_session_t *session)
{
    int failed = 0;
    mtfs_error_t reader_result;
    if (session->model_initialized != 0U &&
        mtfs_model_close(&session->model) != MTFS_OK)
        failed = 1;
    session->model_initialized = 0U;
    if (session->reader_open != 0U) {
        reader_result = mtfs_sealed_reader_fatfs_close(&session->fatfs);
        if (reader_result != MTFS_OK && reader_result != MTFS_ERROR_NO_MEDIA &&
            reader_result != MTFS_ERROR_NOT_READY)
            failed = 1;
    }
    session->reader_open = 0U;
    if (session->provider_open != 0U &&
        mtfs_stm32_saes_provider_deinit(&session->provider_context) !=
            MTFS_CRYPTO_OK)
        failed = 1;
    session->provider_open = 0U;
    if (session->mutex_id > 0 && tk_del_mtx(session->mutex_id) != E_OK)
        failed = 1;
    session->mutex_id = 0;
    if (session->mounted != 0U && f_mount(NULL, "0:", 0U) != FR_OK)
        failed = 1;
    session->mounted = 0U;
    return failed;
}

static int parse_layout(model_session_t *session,
    mtfs_sealed_package_info_t *info, mtfs_sealed_layout_t *layout)
{
    uint8_t preamble[MTFS_SEALED_PREAMBLE_SIZE];
    size_t received = 0U;
    mtfs_error_t result = session->reader.read_at(session->reader.context,
        0U, preamble, sizeof(preamble), &received);
    if (result == MTFS_OK && received == sizeof(preamble))
        result = mtfs_sealed_format_parse(preamble, info, layout);
    if (result == MTFS_OK)
        result = mtfs_sealed_format_finish_layout(info, layout);
    mtfs_secure_zero(preamble, sizeof(preamble));
    return result == MTFS_OK ? 0 : 1;
}

static int open_model(model_session_t *session,
    const mtfs_sealed_reader_t *reader, const mtfs_model_policy_t *policy)
{
    mtfs_sealed_work_t work = model_work();
    return mtfs_model_open(&session->model, reader, &session->provider,
        &work, policy) == MTFS_OK ? 0 : 1;
}

static int run_info(const mtfs_media_context_t *media)
{
    model_session_t session;
    mtfs_model_policy_t policy = model_policy();
    mtfs_model_info_t info;
    int stage = session_setup(&session, media);
    int failed = stage != 0;
    if (!failed)
        failed = open_model(&session, &session.reader, &policy);
    if (!failed)
        failed = mtfs_model_get_info(&session.model, &info) != MTFS_OK;
    if (!failed) {
        tm_printf((UB *)"[model-info] PASS version=%u payload=%u required-ram=%u chunk=%u count=%u target=%u accelerator=%u format=test-only-%u\n",
            (UW)info.model_version, (UW)info.payload_size,
            (UW)info.required_ram, info.chunk_size, info.chunk_count,
            info.target_id, info.accelerator_id, info.model_format);
    }
    failed |= session_cleanup(&session);
    if (failed)
        tm_printf((UB *)"[model-info] FAIL stage=%d\n", stage);
    return failed;
}

static int run_load(const mtfs_media_context_t *media)
{
    model_session_t session;
    mtfs_model_policy_t policy = model_policy();
    size_t loaded = 0U;
    int stage = session_setup(&session, media);
    int failed = stage != 0;
    memset(destination, 0xa5, sizeof(destination));
    if (!failed)
        failed = open_model(&session, &session.reader, &policy);
    if (!failed)
        failed = mtfs_model_load(&session.model, destination,
            sizeof(destination), &loaded) != MTFS_OK;
    if (!failed)
        failed = loaded != MODEL_PAYLOAD_BYTES || !known_payload(destination) ||
            !all_value(destination + MODEL_PAYLOAD_BYTES, MODEL_EXTRA_BYTES,
                0xa5U) || !work_is_zero();
    failed |= session_cleanup(&session);
    tm_printf((UB *)"[model-load] %s payload=5000 exact=%s extra-preserved=%s work-zero=%s cleanup=%s\n",
        failed ? (UB *)"FAIL" : (UB *)"PASS",
        known_payload(destination) ? (UB *)"PASS" : (UB *)"FAIL",
        all_value(destination + MODEL_PAYLOAD_BYTES, MODEL_EXTRA_BYTES, 0xa5U)
            ? (UB *)"PASS" : (UB *)"FAIL",
        work_is_zero() ? (UB *)"PASS" : (UB *)"FAIL",
        failed ? (UB *)"CHECK" : (UB *)"PASS");
    return failed;
}

typedef enum mutation_kind
{
    MUTATE_ENVELOPE_TAG,
    MUTATE_PAYLOAD_CIPHERTEXT,
    MUTATE_PAYLOAD_TAG
} mutation_kind_t;

static int run_mutation(const mtfs_media_context_t *media,
    mutation_kind_t kind)
{
    model_session_t session;
    mtfs_model_policy_t policy = model_policy();
    mtfs_sealed_package_info_t package_info;
    mtfs_sealed_layout_t layout;
    uint64_t mutation;
    size_t loaded = 123U;
    mtfs_error_t result;
    int stage = session_setup(&session, media);
    int failed = stage != 0;
    memset(destination, 0xa5, sizeof(destination));
    if (!failed)
        failed = parse_layout(&session, &package_info, &layout);
    if (failed) {
        (void)session_cleanup(&session);
        return 1;
    }
    mutation = layout.envelope_offset + 32U;
    if (kind == MUTATE_PAYLOAD_CIPHERTEXT)
        mutation = layout.payload_offset + 17U;
    else if (kind == MUTATE_PAYLOAD_TAG)
        mutation = layout.payload_offset + package_info.chunk_plain_size + 5U;
    decorate(&session, mutation, layout.payload_offset, 1);
    result = mtfs_model_open(&session.model, &session.decorated_reader,
        &session.provider, &(mtfs_sealed_work_t){
            MTFS_SEALED_WORK_API_VERSION, sizeof(mtfs_sealed_work_t),
            manifest_work, sizeof(manifest_work), aad_work, sizeof(aad_work),
            ciphertext_work, sizeof(ciphertext_work), plaintext_work,
            sizeof(plaintext_work)}, &policy);
    if (kind == MUTATE_ENVELOPE_TAG) {
        failed = result != MTFS_ERROR_AUTHENTICATION ||
            session.model.state != MTFS_MODEL_ERROR || !work_is_zero() ||
            session.provider_context.fleet_open != 0U ||
            session.provider_context.model_open != 0U;
    } else {
        if (result != MTFS_OK)
            failed = 1;
        else {
            result = mtfs_model_load(&session.model, destination,
                sizeof(destination), &loaded);
            failed = result != MTFS_ERROR_AUTHENTICATION || loaded != 0U ||
                !all_value(destination, MODEL_PAYLOAD_BYTES, 0U) ||
                !all_value(destination + MODEL_PAYLOAD_BYTES,
                    MODEL_EXTRA_BYTES, 0xa5U) || !work_is_zero() ||
                session.provider_context.fleet_open != 0U ||
                session.provider_context.model_open != 0U ||
                session.model.state != MTFS_MODEL_ERROR;
        }
    }
    failed |= session_cleanup(&session);
    return failed;
}

static int run_policy_case(const mtfs_media_context_t *media, unsigned int kind)
{
    model_session_t session;
    mtfs_model_policy_t policy = model_policy();
    mtfs_sealed_package_info_t package_info;
    mtfs_sealed_layout_t layout;
    mtfs_model_info_t info;
    mtfs_model_info_t sentinel;
    mtfs_error_t result;
    int failed = session_setup(&session, media) != 0;
    if (!failed)
        failed = parse_layout(&session, &package_info, &layout);
    if (failed) {
        (void)session_cleanup(&session);
        return 1;
    }
    if (kind == 0U)
        ++policy.expected_target_id;
    else if (kind == 1U)
        ++policy.expected_accelerator_id;
    else
        ++policy.accepted_model_format;
    decorate(&session, 0U, layout.payload_offset, 0);
    memset(&info, 0xa5, sizeof(info));
    sentinel = info;
    result = mtfs_model_open(&session.model, &session.decorated_reader,
        &session.provider, &(mtfs_sealed_work_t){
            MTFS_SEALED_WORK_API_VERSION, sizeof(mtfs_sealed_work_t),
            manifest_work, sizeof(manifest_work), aad_work, sizeof(aad_work),
            ciphertext_work, sizeof(ciphertext_work), plaintext_work,
            sizeof(plaintext_work)}, &policy);
    failed = result != (kind == 2U ? MTFS_ERROR_UNSUPPORTED_FORMAT :
        MTFS_ERROR_NOT_SUPPORTED) || session.decorator.payload_reads != 0U ||
        mtfs_model_get_info(&session.model, &info) != MTFS_ERROR_INVALID_STATE ||
        memcmp(&info, &sentinel, sizeof(info)) != 0 || !work_is_zero();
    failed |= session_cleanup(&session);
    return failed;
}

static int run_negative(const mtfs_media_context_t *media)
{
    int envelope = run_mutation(media, MUTATE_ENVELOPE_TAG);
    int ciphertext = run_mutation(media, MUTATE_PAYLOAD_CIPHERTEXT);
    int tag = run_mutation(media, MUTATE_PAYLOAD_TAG);
    int target = run_policy_case(media, 0U);
    int accelerator = run_policy_case(media, 1U);
    int format = run_policy_case(media, 2U);
    int failed = envelope || ciphertext || tag || target || accelerator || format;
    tm_printf((UB *)"[model-negative] envelope-tag=%s payload-ciphertext=%s payload-tag=%s target-policy=%s accelerator-policy=%s format-policy=%s SD=unchanged overall=%s\n",
        envelope ? (UB *)"FAIL" : (UB *)"PASS",
        ciphertext ? (UB *)"FAIL" : (UB *)"PASS",
        tag ? (UB *)"FAIL" : (UB *)"PASS",
        target ? (UB *)"FAIL" : (UB *)"PASS",
        accelerator ? (UB *)"FAIL" : (UB *)"PASS",
        format ? (UB *)"FAIL" : (UB *)"PASS",
        failed ? (UB *)"FAIL" : (UB *)"PASS");
    return failed;
}

static int wait_media(const mtfs_media_context_t *media, int present)
{
    uint32_t count;
    for (count = 0U; count < HOTPLUG_WAIT_TICKS; ++count) {
        if (!!mtfs_media_is_present(media) == !!present)
            return 0;
        (void)tk_dly_tsk(10U);
    }
    return 1;
}

static int run_hotplug(const mtfs_media_context_t *media)
{
    model_session_t session;
    mtfs_model_policy_t policy = model_policy();
    size_t loaded = 999U;
    mtfs_error_t result;
    int failed = session_setup(&session, media) != 0;
    memset(destination, 0xa5, sizeof(destination));
    if (!failed)
        failed = open_model(&session, &session.reader, &policy);
    if (!failed) {
        tm_printf((UB *)"[model-hotplug] ACTION REQUIRED: REMOVE card after authenticated open\n");
        failed = wait_media(media, 0);
    }
    if (!failed) {
        result = mtfs_model_load(&session.model, destination,
            sizeof(destination), &loaded);
        failed = (result != MTFS_ERROR_NO_MEDIA &&
            result != MTFS_ERROR_NOT_READY) || loaded != 0U ||
            !all_value(destination, MODEL_PAYLOAD_BYTES, 0U) ||
            !all_value(destination + MODEL_PAYLOAD_BYTES, MODEL_EXTRA_BYTES,
                0xa5U) || !work_is_zero();
    }
    failed |= session_cleanup(&session);
    tm_printf((UB *)"[model-hotplug] ACTION REQUIRED: REINSERT card for explicit reinitialize/mount\n");
    if (!failed)
        failed = wait_media(media, 1);
    if (!failed && disk_initialize(0U) != 0U)
        failed = 1;
    if (!failed)
        failed = run_load(media);
    tm_printf((UB *)"[model-hotplug] %s remove-cleanup=%s recovery=%s\n",
        failed ? (UB *)"FAIL" : (UB *)"PASS",
        failed ? (UB *)"CHECK" : (UB *)"PASS",
        failed ? (UB *)"FAIL" : (UB *)"PASS");
    return failed;
}

int mtfs_stm32n6570_model_command(const char *line,
    const mtfs_media_context_t *media)
{
    if (strcmp(line, "model-info") == 0) {
        (void)run_info(media);
        return 1;
    }
    if (strcmp(line, "model-load") == 0) {
        (void)run_load(media);
        return 1;
    }
    if (strcmp(line, "model-negative") == 0) {
        (void)run_negative(media);
        return 1;
    }
    if (strcmp(line, "model-hotplug") == 0) {
        (void)run_hotplug(media);
        return 1;
    }
    return 0;
}
