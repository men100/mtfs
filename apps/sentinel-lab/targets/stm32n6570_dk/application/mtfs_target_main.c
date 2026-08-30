#include <stddef.h>
#include <string.h>
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <mtkernel/lib/libtm/libtm.h>
#include "ff.h"
#include "mtfs_block_registry.h"
#include "mtfs_media.h"
#include "mtfs_media_service.h"
#include "mtfs_sentinel.h"
#include "mtfs_sentinel_lab_console.h"
#include "mtfs_sentinel_recorder.h"
#include "mtfs_stm32_sdmmc.h"
#include "mtfs_stm32n6570_dk_platform.h"

#define LAB_TARGET_ID (UINT32_C(0x53544e36))
#if MTFS_STM32_SD_USE_IDMA
#define LAB_TRANSPORT_ID (UINT32_C(0x49444d41))
#define LAB_TRANSPORT_NAME "IDMA"
#else
#define LAB_TRANSPORT_ID (UINT32_C(0x504f4c4c))
#define LAB_TRANSPORT_NAME "polling"
#endif
#define LAB_RECORD_INTERVAL_MS (1000U)
#define LAB_CSV_LINE_BYTES (4096U)
#ifndef MTFS_SENTINEL_RECORDER_LABEL
#define MTFS_SENTINEL_RECORDER_LABEL "controlled"
#endif

static mtfs_stm32_sdmmc_context_t sd_context;
static mtfs_media_context_t media_context;
static mtfs_media_service_context_t media_service;
#if MTFS_ENABLE_STORAGE_SENTINEL
static mtfs_sentinel_observer_t observer;
static mtfs_sentinel_context_t sentinel;
static mtfs_sentinel_feature_v1_t frame;
#endif
static FATFS filesystem;
static uint8_t workload_buffer[4096];
#if MTFS_ENABLE_STORAGE_SENTINEL
static char csv_line[LAB_CSV_LINE_BYTES];
static ID observer_mutex_id;
#endif

#if MTFS_ENABLE_STORAGE_SENTINEL
static int sentinel_runtime_self_test(void)
{
    mtfs_sentinel_operation_feature_t operation;
    (void)memset(&operation, 0, sizeof(operation));
    operation.timing_samples = UINT64_C(1);
    operation.total_latency_us = UINT64_C(513);
    operation.average_latency_us = UINT64_C(513);
    operation.latency_histogram[10] = UINT64_C(1);
    if (mtfs_sentinel_histogram_bucket(UINT64_C(512)) != 9U ||
        mtfs_sentinel_histogram_bucket(UINT64_C(513)) != 10U ||
        mtfs_sentinel_histogram_bucket(UINT64_C(651)) != 10U ||
        mtfs_sentinel_histogram_bucket(UINT64_C(9766)) != 14U ||
        !mtfs_sentinel_operation_timing_is_consistent(&operation))
        return 0;
    operation.latency_histogram[10] = 0U;
    operation.latency_histogram[9] = UINT64_C(1);
    return !mtfs_sentinel_operation_timing_is_consistent(&operation);
}

static uint32_t sentinel_frame_timing_mask(
    const mtfs_sentinel_feature_v1_t *feature)
{
    uint32_t operation;
    uint32_t mask = 0U;
    for (operation = 0U; operation < MTFS_SENTINEL_OPERATION_COUNT;
         ++operation) {
        if (mtfs_sentinel_operation_timing_is_consistent(
                &feature->operation[operation]))
            mask |= UINT32_C(1) << operation;
    }
    return mask;
}

static mtfs_error_t observer_lock(void *opaque)
{
    return tk_loc_mtx(*(ID *)opaque, TMO_FEVR) >= E_OK ?
        MTFS_OK : MTFS_ERROR_NOT_READY;
}
static void observer_unlock(void *opaque) { (void)tk_unl_mtx(*(ID *)opaque); }

static mtfs_error_t transport_sample(void *opaque,
    mtfs_sentinel_transport_snapshot_t *snapshot)
{
    mtfs_stm32_sdmmc_diagnostics_t d;
    mtfs_error_t error = mtfs_stm32_sdmmc_diagnostics_get(opaque, &d);
    if (error != MTFS_OK) return error;
    snapshot->reset_epoch = d.reset_epoch;
    snapshot->validity_mask =
        MTFS_SENTINEL_TRANSPORT_VALID_TRANSPORT_ERRORS |
        MTFS_SENTINEL_TRANSPORT_VALID_TRANSFER_TIMEOUTS |
        MTFS_SENTINEL_TRANSPORT_VALID_READY_TIMEOUTS |
        MTFS_SENTINEL_TRANSPORT_VALID_ABORTS;
    snapshot->flags = MTFS_SENTINEL_TRANSPORT_FLAG_COUNTERS_SATURATE;
    snapshot->transport_errors = d.error_callbacks;
    snapshot->transfer_timeouts = d.completion_timeouts;
    snapshot->ready_timeouts = d.card_state_timeouts;
    snapshot->aborts = d.aborts;
    if (d.error_callbacks == UINT32_MAX ||
        d.completion_timeouts == UINT32_MAX ||
        d.card_state_timeouts == UINT32_MAX || d.aborts == UINT32_MAX)
        snapshot->flags |=
            MTFS_SENTINEL_TRANSPORT_FLAG_COUNTER_SATURATED;
    return MTFS_OK;
}

static void collect_metadata(mtfs_sentinel_sample_metadata_t *metadata)
{
    mtfs_media_diagnostics_t d;
    if (mtfs_media_diagnostics_get(&media_context, &d) == MTFS_OK) {
        metadata->media_generation = d.media_generation;
        metadata->media_reset_epoch = d.reset_epoch;
        metadata->inserted_events = d.inserted_events;
        metadata->removed_events = d.removed_events;
        metadata->error_events = d.error_events;
    }
}
#endif

static void print_workload_performance(uint32_t marker, uint64_t start_us,
    uint64_t end_us)
{
    uint64_t elapsed_us;
    if (end_us < start_us || end_us - start_us > UINT32_MAX) {
        tm_printf((UB *)
            "# workload-perf marker=%u elapsed_us=invalid sentinel=%u\n",
            marker, (uint32_t)MTFS_ENABLE_STORAGE_SENTINEL);
        return;
    }
    elapsed_us = end_us - start_us;
    tm_printf((UB *)
        "# workload-perf marker=%u elapsed_us=%u sentinel=%u\n",
        marker, (uint32_t)elapsed_us,
        (uint32_t)MTFS_ENABLE_STORAGE_SENTINEL);
}

static int run_record(void)
{
    mtfs_stm32_sdmmc_config_t sd_config;
#if MTFS_ENABLE_STORAGE_SENTINEL
    T_CMTX mutex = {.mtxatr = TA_INHERIT};
    mtfs_sentinel_observer_config_t oc;
    mtfs_sentinel_config_t sc;
    mtfs_sentinel_sample_metadata_t metadata = {0};
    mtfs_error_t sample_error;
    uint32_t timing_mask;
#endif
    mtfs_block_device_t *device;
    mtfs_error_t workload_error;
    uint64_t workload_start_us;
    uint64_t workload_end_us;
    uint32_t marker = 1U;
#if MTFS_ENABLE_STORAGE_SENTINEL
    observer_mutex_id = tk_cre_mtx(&mutex);
    if (observer_mutex_id <= 0) return 1;
#endif
    mtfs_stm32n6570_dk_sdmmc_config(&sd_config);
    if (mtfs_stm32_sdmmc_context_init(&sd_context, &sd_config) != MTFS_OK ||
        mtfs_stm32n6570_dk_card_detect_start(&media_context, &media_service,
            &sd_context, NULL, NULL) != MTFS_OK) return 1;
    device = mtfs_stm32_sdmmc_block_device(&sd_context);
#if MTFS_ENABLE_STORAGE_SENTINEL
    oc.downstream = device;
    oc.clock = mtfs_stm32n6570_dk_sentinel_clock_us;
    oc.clock_context = NULL;
    oc.lock = observer_lock;
    oc.unlock = observer_unlock;
    oc.lock_context = &observer_mutex_id;
    if (mtfs_sentinel_observer_init(&observer, &oc) != MTFS_OK) return 1;
    device = mtfs_sentinel_observer_block_device(&observer);
#endif
    if (mtfs_block_initialize(device) != MTFS_OK ||
        mtfs_block_registry_register(0U, device) != MTFS_OK ||
        f_mount(&filesystem, "0:", 1U) != FR_OK) return 1;
#if MTFS_ENABLE_STORAGE_SENTINEL
    sc.observer = &observer;
    sc.clock = mtfs_stm32n6570_dk_sentinel_clock_us;
    sc.clock_context = NULL;
    sc.target_id = LAB_TARGET_ID;
    sc.transport_id = LAB_TRANSPORT_ID;
    sc.transport_sample = transport_sample;
    sc.transport_context = &sd_context;
    if (mtfs_sentinel_init(&sentinel, &sc) != MTFS_OK) return 1;
    collect_metadata(&metadata);
    (void)mtfs_sentinel_sample(&sentinel, &metadata, &frame);
    tm_printf((UB *)"%s\n", (UB *)mtfs_sentinel_recorder_csv_header());
#else
    tm_printf((UB *)"# sentinel: disabled performance baseline\n");
#endif
    for (;;) {
        workload_start_us = mtfs_stm32n6570_dk_benchmark_clock_us(NULL);
        workload_error = mtfs_sentinel_recorder_workload("0:", marker,
            workload_buffer, sizeof(workload_buffer));
        workload_end_us = mtfs_stm32n6570_dk_benchmark_clock_us(NULL);
        print_workload_performance(marker, workload_start_us,
            workload_end_us);
        (void)tk_dly_tsk(LAB_RECORD_INTERVAL_MS);
#if MTFS_ENABLE_STORAGE_SENTINEL
        collect_metadata(&metadata);
        sample_error = mtfs_sentinel_sample(&sentinel, &metadata, &frame);
        if (sample_error == MTFS_OK) {
            timing_mask = sentinel_frame_timing_mask(&frame);
            if (timing_mask != UINT32_C(0x07))
                tm_printf((UB *)
                    "# post-sample timing invariant marker=%u mask=%u flags=%u\n",
                    marker, timing_mask, frame.flags);
            if (mtfs_sentinel_recorder_format_csv(csv_line, sizeof(csv_line),
                    &frame, MTFS_SENTINEL_RECORDER_LABEL, marker) == MTFS_OK)
                tm_printf((UB *)"%s\n", (UB *)csv_line);
        }
#endif
        if (workload_error != MTFS_OK)
            tm_printf((UB *)"# workload_error marker=%u mtfs=%d\n",
                marker, workload_error);
        ++marker;
    }
}

static void lab_console_write(void *context, const char *text)
{
    INT length = 0;
    (void)context;
    if (text == NULL) return;
    while (text[length] != '\0') ++length;
    if (length != 0) tm_snd_dat((const UB *)text, length);
}

static int lab_command(void *context, const char *line)
{
    (void)context;
    if (strcmp(line, "help") == 0) {
#if MTFS_ENABLE_STORAGE_SENTINEL
        lab_console_write(NULL,
            "help    show this help\r\n"
            "record  collect CSV continuously until board reset\r\n");
#else
        lab_console_write(NULL,
            "help    show this help\r\n"
            "record  measure workload continuously until board reset\r\n");
#endif
        return 1;
    }
    if (strcmp(line, "record") == 0) {
        tm_printf((UB *)"# record: continuous until board reset\n");
        tm_printf((UB *)"# record_exit=%d\n", run_record());
        return 1;
    }
    return 0;
}

static void lab_task(INT start_code, void *context)
{
    mtfs_sentinel_lab_console_t console;
    (void)start_code;
    (void)context;
    mtfs_sentinel_lab_console_init(&console, lab_console_write, NULL,
        lab_command, NULL);
    tm_printf((UB *)"\nmicroT-FS Storage Sentinel Lab\n");
    tm_printf((UB *)"# target: STM32N6570-DK\n");
    tm_printf((UB *)"# transport: %s\n", (UB *)LAB_TRANSPORT_NAME);
#if MTFS_ENABLE_STORAGE_SENTINEL
    tm_printf((UB *)"# feature schema: v%u\n", MTFS_SENTINEL_SCHEMA_VERSION);
    tm_printf((UB *)"# arithmetic: portable-u64-v3\n");
    tm_printf((UB *)"# sentinel self-test: %s\n",
        (UB *)(sentinel_runtime_self_test() ? "PASS" : "FAIL"));
#else
    tm_printf((UB *)"# sentinel: disabled performance baseline\n");
#endif
    tm_printf((UB *)"# sample interval: %u ms\n", LAB_RECORD_INTERVAL_MS);
    tm_printf((UB *)"# record writes and removes temporary files continuously\n");
    tm_printf((UB *)"# insert a FAT-formatted SD card before recording\n");
    lab_console_write(NULL, "Type help for commands.\r\n> ");
    for (;;)
        mtfs_sentinel_lab_console_feed(&console, (char)tm_getchar(1));
}

EXPORT INT usermain(void)
{
    T_CTSK task = {.tskatr=TA_HLNG|TA_RNG3,.task=lab_task,
        .itskpri=9,.stksz=12U*1024U};
    ID task_id = tk_cre_tsk(&task);
    if ((task_id <= 0) || (tk_sta_tsk(task_id, 0) < E_OK))
        tm_printf((UB *)"# sentinel-lab task start failed\n");
    for (;;) (void)tk_slp_tsk(TMO_FEVR);
    return 0;
}
