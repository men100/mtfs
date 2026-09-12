#include "mtfs_target_concurrent.h"

#include <stddef.h>
#include <string.h>

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "ff.h"

#define TARGET_WORKERS       (2U)
#define TARGET_CHUNK_SIZE    (512U)
#define TARGET_ITERATIONS    (8U)
#define TARGET_WORKER_STACK_SIZE (16U * 1024U)
#define TARGET_STACK_GUARD_SIZE  (32U)
#define TARGET_STACK_FILL        (0xA5U)
#define TARGET_START_BIT     (UINT32_C(1) << 0)
#define TARGET_READY_BIT(i)  (UINT32_C(1) << (1U + (i)))
#define TARGET_DONE_BIT(i)   (UINT32_C(1) << (3U + (i)))
#define TARGET_READY_MASK    (TARGET_READY_BIT(0) | TARGET_READY_BIT(1))
#define TARGET_DONE_MASK     (TARGET_DONE_BIT(0) | TARGET_DONE_BIT(1))
#define TARGET_WAIT_MS       (30000)

typedef struct target_worker
{
    unsigned int index;
    unsigned int seed;
    const char *path;
    ID event_flag_id;
    FRESULT result;
    UINT transferred;
    unsigned int failed_iteration;
    int file_open;
    FIL file;
    BYTE write_buffer[TARGET_CHUNK_SIZE];
    BYTE read_buffer[TARGET_CHUNK_SIZE];
} target_worker_t;

static target_worker_t workers[TARGET_WORKERS];
static FATFS concurrent_filesystem;
typedef struct target_worker_stack
{
    UB guard[TARGET_STACK_GUARD_SIZE];
    UW stack[TARGET_WORKER_STACK_SIZE / sizeof(UW)];
} target_worker_stack_t;

static target_worker_stack_t worker_stacks[TARGET_WORKERS]
    __attribute__((aligned(8)));
static size_t worker_peak_used[TARGET_WORKERS];
static int worker_guard_ok[TARGET_WORKERS];
static int worker_watermark_valid[TARGET_WORKERS];

/* Cortex-M task stacks descend toward the guard at the buffer's low end. */
static size_t target_stack_free_bytes(const UW *stack, size_t stack_size)
{
    const volatile UB *bytes = (const volatile UB *)stack;
    size_t free_bytes = 0U;

    while ((free_bytes < stack_size) &&
        (bytes[free_bytes] == (UB)TARGET_STACK_FILL)) {
        ++free_bytes;
    }
    return free_bytes;
}

static int target_stack_guard_ok(const UB *guard, size_t guard_size)
{
    const volatile UB *bytes = (const volatile UB *)guard;
    size_t i;

    for (i = 0U; i < guard_size; ++i) {
        if (bytes[i] != (UB)TARGET_STACK_FILL) {
            return 0;
        }
    }
    return 1;
}

static void target_worker_stack_prepare(unsigned int index)
{
    memset(worker_stacks[index].guard, TARGET_STACK_FILL,
        sizeof(worker_stacks[index].guard));
    memset(worker_stacks[index].stack, TARGET_STACK_FILL,
        sizeof(worker_stacks[index].stack));
}

static void target_worker_stack_record(unsigned int index)
{
    size_t free_bytes = target_stack_free_bytes(worker_stacks[index].stack,
        sizeof(worker_stacks[index].stack));
    size_t used_bytes = sizeof(worker_stacks[index].stack) - free_bytes;
    int guard_ok = target_stack_guard_ok(worker_stacks[index].guard,
        sizeof(worker_stacks[index].guard));

    /* Keep the worst result across every round executed since boot. */
    if (!worker_watermark_valid[index] ||
        (used_bytes > worker_peak_used[index])) {
        worker_peak_used[index] = used_bytes;
    }
    if (!worker_watermark_valid[index]) {
        worker_guard_ok[index] = guard_ok;
    } else {
        worker_guard_ok[index] = worker_guard_ok[index] && guard_ok;
    }
    worker_watermark_valid[index] = 1;
}

unsigned int mtfs_target_concurrent_stack_count(void)
{
    return TARGET_WORKERS;
}

int mtfs_target_concurrent_stack_watermark(unsigned int index,
    mtfs_target_stack_watermark_t *watermark)
{
    if ((index >= TARGET_WORKERS) || (watermark == NULL)) {
        return 0;
    }
    watermark->total_bytes = sizeof(worker_stacks[index].stack);
    watermark->used_bytes = worker_peak_used[index];
    watermark->free_bytes = sizeof(worker_stacks[index].stack) -
        worker_peak_used[index];
    watermark->guard_ok = worker_guard_ok[index];
    watermark->measured = worker_watermark_valid[index];
    return 1;
}

static void target_fill(BYTE *buffer, unsigned int seed, unsigned int iteration)
{
    unsigned int i;
    for (i = 0U; i < TARGET_CHUNK_SIZE; ++i) {
        buffer[i] = (BYTE)((seed + iteration * 17U + i * 37U) & 0xFFU);
    }
}

static void target_worker_task(INT start_code, void *opaque)
{
    target_worker_t *worker = (target_worker_t *)opaque;
    UINT flags;
    unsigned int iteration;

    (void)start_code;
    worker->result = FR_INT_ERR;
    (void)tk_set_flg(worker->event_flag_id, TARGET_READY_BIT(worker->index));
    if (tk_wai_flg(worker->event_flag_id, TARGET_START_BIT, TWF_ORW,
            &flags, TARGET_WAIT_MS) < E_OK) {
        goto done;
    }

    worker->result = f_open(&worker->file, worker->path,
        FA_CREATE_ALWAYS | FA_WRITE);
    if (worker->result != FR_OK) {
        goto done;
    }
    worker->file_open = 1;
    for (iteration = 0U; iteration < TARGET_ITERATIONS; ++iteration) {
        target_fill(worker->write_buffer, worker->seed, iteration);
        worker->transferred = 0U;
        worker->result = f_write(&worker->file, worker->write_buffer,
            (UINT)sizeof(worker->write_buffer), &worker->transferred);
        if ((worker->result != FR_OK) ||
            (worker->transferred != (UINT)sizeof(worker->write_buffer))) {
            worker->failed_iteration = iteration;
            break;
        }
        (void)tk_dly_tsk(1U);
    }
    if (worker->result == FR_OK) {
        worker->result = f_sync(&worker->file);
    }
    {
        FRESULT close_result = f_close(&worker->file);
        worker->file_open = 0;
        if (worker->result == FR_OK) {
            worker->result = close_result;
        }
    }
    if (worker->result != FR_OK) {
        goto done;
    }
    worker->result = f_open(&worker->file, worker->path, FA_READ);
    if (worker->result != FR_OK) {
        goto done;
    }
    worker->file_open = 1;
    for (iteration = 0U; iteration < TARGET_ITERATIONS; ++iteration) {
        target_fill(worker->write_buffer, worker->seed, iteration);
        worker->transferred = 0U;
        worker->result = f_read(&worker->file, worker->read_buffer,
            (UINT)sizeof(worker->read_buffer), &worker->transferred);
        if ((worker->result != FR_OK) ||
            (worker->transferred != (UINT)sizeof(worker->read_buffer)) ||
            (memcmp(worker->write_buffer, worker->read_buffer,
                sizeof(worker->read_buffer)) != 0)) {
            worker->result = FR_INT_ERR;
            worker->failed_iteration = iteration;
            break;
        }
    }
    {
        FRESULT close_result = f_close(&worker->file);
        worker->file_open = 0;
        if (worker->result == FR_OK) {
            worker->result = close_result;
        }
    }

done:
    (void)tk_set_flg(worker->event_flag_id, TARGET_DONE_BIT(worker->index));
    /*
     * Stay parked until the coordinator terminates this task.  In particular,
     * do not race coordinator cleanup by returning through tk_ext_tsk() while
     * the coordinator may already be deleting the shared event flag.
     */
    for (;;) {
        (void)tk_slp_tsk(TMO_FEVR);
    }
}

static int target_verify_persisted(mtfs_test_t *test, target_worker_t *worker)
{
    unsigned int iteration;
    FRESULT result;
    UINT transferred;
    int ok = 1;

    result = f_open(&worker->file, worker->path, FA_READ);
    if (!MTFS_TEST_CHECK(test, result == FR_OK,
            "open concurrent file after remount")) {
        return 0;
    }
    for (iteration = 0U; iteration < TARGET_ITERATIONS; ++iteration) {
        target_fill(worker->write_buffer, worker->seed, iteration);
        transferred = 0U;
        result = f_read(&worker->file, worker->read_buffer,
            (UINT)sizeof(worker->read_buffer), &transferred);
        if ((result != FR_OK) ||
            (transferred != (UINT)sizeof(worker->read_buffer)) ||
            (memcmp(worker->write_buffer, worker->read_buffer,
                sizeof(worker->read_buffer)) != 0)) {
            ok = 0;
            break;
        }
    }
    (void)MTFS_TEST_CHECK(test, ok, "concurrent file persisted content matches");
    (void)MTFS_TEST_CHECK(test, f_close(&worker->file) == FR_OK,
        "close concurrent file after verification");
    return ok;
}

int mtfs_target_run_concurrent(mtfs_test_t *test, const char *volume_path,
    unsigned int outer_iteration)
{
    static const char *const paths[TARGET_WORKERS] = {
#if MTFS_FF_ENABLE_LFN
        "0:microtfs concurrent task alpha result.bin",
        "0:microtfs concurrent task beta result.bin"
#else
        "0:TASKA.BIN", "0:TASKB.BIN"
#endif
    };
    static const unsigned int seeds[TARGET_WORKERS] = {0x31U, 0xA7U};
    T_CFLG flag_config = {
        .exinf = NULL,
        .flgatr = TA_TFIFO | TA_WMUL,
        .iflgptn = 0U
    };
    T_CTSK task_config = {
        .exinf = NULL,
        .tskatr = TA_HLNG | TA_RNG3 | TA_USERBUF,
        .task = target_worker_task,
        .itskpri = 10,
        .stksz = TARGET_WORKER_STACK_SIZE,
        .bufptr = NULL
    };
    ID task_ids[TARGET_WORKERS] = {0, 0};
    int task_started[TARGET_WORKERS] = {0, 0};
    ID event_flag_id;
    UINT flags;
    FRESULT fat_result;
    unsigned int i;
    int mounted = 0;
    int result = 1;

    if ((test == NULL) || (volume_path == NULL)) {
        return result;
    }
    fat_result = f_mount(&concurrent_filesystem, volume_path, 1U);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK,
            "mount prepared volume for target concurrent access")) {
        return result;
    }
    mounted = 1;
    event_flag_id = tk_cre_flg(&flag_config);
    if (!MTFS_TEST_CHECK(test, event_flag_id > 0,
            "create concurrent coordination event flag")) {
        goto cleanup;
    }

    memset(workers, 0, sizeof(workers));
    for (i = 0U; i < TARGET_WORKERS; ++i) {
        target_worker_stack_prepare(i);
        workers[i].index = i;
        workers[i].seed = seeds[i];
        workers[i].path = paths[i];
        workers[i].event_flag_id = event_flag_id;
        task_config.exinf = &workers[i];
        task_config.bufptr = worker_stacks[i].stack;
        task_ids[i] = tk_cre_tsk(&task_config);
        if (task_ids[i] <= 0) {
            (void)MTFS_TEST_CHECK(test, 0, "create both writer tasks");
            goto cleanup_tasks;
        }
        if (tk_sta_tsk(task_ids[i], 0) < E_OK) {
            (void)MTFS_TEST_CHECK(test, 0, "create and start both writer tasks");
            goto cleanup_tasks;
        }
        task_started[i] = 1;
    }
    if (!MTFS_TEST_CHECK(test,
            tk_wai_flg(event_flag_id, TARGET_READY_MASK, TWF_ANDW,
                &flags, TARGET_WAIT_MS) >= E_OK,
            "wait until both writer tasks are ready")) {
        goto cleanup_tasks;
    }
    (void)tk_set_flg(event_flag_id, TARGET_START_BIT);
    if (!MTFS_TEST_CHECK(test,
            tk_wai_flg(event_flag_id, TARGET_DONE_MASK, TWF_ANDW,
                &flags, TARGET_WAIT_MS) >= E_OK,
            "wait for both writer tasks to finish")) {
        goto cleanup_tasks;
    }
    (void)tk_dly_tsk(1U);
    for (i = 0U; i < TARGET_WORKERS; ++i) {
        if (!MTFS_TEST_CHECK(test, workers[i].result == FR_OK,
                "worker write and immediate read-back succeeded")) {
            tm_printf((UB *)"[mtfs] round %u worker %u FAIL: fresult=%u inner=%u\n",
                outer_iteration, i, workers[i].result,
                workers[i].failed_iteration);
            goto cleanup_tasks;
        }
    }
    fat_result = f_mount(NULL, volume_path, 0U);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK,
            "unmount after target concurrent writes")) {
        goto cleanup_tasks;
    }
    mounted = 0;
    fat_result = f_mount(&concurrent_filesystem, volume_path, 1U);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK,
            "remount after target concurrent writes")) {
        goto cleanup_tasks;
    }
    mounted = 1;
    for (i = 0U; i < TARGET_WORKERS; ++i) {
        if (!target_verify_persisted(test, &workers[i])) {
            goto cleanup_tasks;
        }
    }
    for (i = 0U; i < TARGET_WORKERS; ++i) {
        if (!MTFS_TEST_CHECK(test, f_unlink(paths[i]) == FR_OK,
                "remove target concurrent test file")) {
            goto cleanup_tasks;
        }
    }
    result = 0;

cleanup_tasks:
    (void)tk_set_flg(event_flag_id, TARGET_START_BIT);
    for (i = 0U; i < TARGET_WORKERS; ++i) {
        if (task_ids[i] > 0) {
            if (task_started[i]) {
                (void)MTFS_TEST_CHECK(test, tk_ter_tsk(task_ids[i]) >= E_OK,
                    "terminate writer task during cleanup");
            }
            if (workers[i].file_open) {
                (void)f_close(&workers[i].file);
                workers[i].file_open = 0;
            }
            if (MTFS_TEST_CHECK(test, tk_del_tsk(task_ids[i]) >= E_OK,
                    "delete writer task during cleanup")) {
                target_worker_stack_record(i);
            }
        }
    }
    (void)MTFS_TEST_CHECK(test, tk_del_flg(event_flag_id) >= E_OK,
        "delete coordination event flag during cleanup");
cleanup:
    if (mounted) {
        if (result != 0) {
            for (i = 0U; i < TARGET_WORKERS; ++i) {
                (void)f_unlink(paths[i]);
            }
        }
        (void)f_mount(NULL, volume_path, 0U);
    }
    return result;
}
