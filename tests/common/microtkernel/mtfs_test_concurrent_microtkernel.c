#include "mtfs_test_concurrent_microtkernel.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tk/tkernel.h>

#include "ff.h"

#define MTFS_TK_WORKERS              (2U)
#define MTFS_TK_CHUNK_SIZE           (512U)
#define MTFS_TK_ITERATIONS           (8U)
#define MTFS_TK_WORKER_STACK_SIZE    (16U * 1024U)
#define MTFS_TK_START_BIT            (UINT32_C(1) << 0)
#define MTFS_TK_READY_BIT(i)         (UINT32_C(1) << (1U + (i)))
#define MTFS_TK_DONE_BIT(i)          (UINT32_C(1) << (3U + (i)))
#define MTFS_TK_READY_MASK           \
    (MTFS_TK_READY_BIT(0) | MTFS_TK_READY_BIT(1))
#define MTFS_TK_DONE_MASK            \
    (MTFS_TK_DONE_BIT(0) | MTFS_TK_DONE_BIT(1))
#define MTFS_TK_WAIT_MS              (30000)

typedef struct mtfs_tk_worker
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
    BYTE write_buffer[MTFS_TK_CHUNK_SIZE];
    BYTE read_buffer[MTFS_TK_CHUNK_SIZE];
} mtfs_tk_worker_t;

static mtfs_tk_worker_t workers[MTFS_TK_WORKERS];
static FATFS concurrent_filesystem;
static UW worker_stacks[MTFS_TK_WORKERS]
    [MTFS_TK_WORKER_STACK_SIZE / sizeof(UW)] __attribute__((aligned(8)));

static void mtfs_tk_fill(BYTE *buffer, unsigned int seed,
    unsigned int iteration)
{
    unsigned int index;
    for (index = 0U; index < MTFS_TK_CHUNK_SIZE; ++index) {
        buffer[index] =
            (BYTE)((seed + iteration * 17U + index * 37U) & 0xFFU);
    }
}

static void mtfs_tk_worker_task(INT start_code, void *opaque)
{
    mtfs_tk_worker_t *worker = opaque;
    UINT flags;
    unsigned int iteration;

    (void)start_code;
    worker->result = FR_INT_ERR;
    (void)tk_set_flg(worker->event_flag_id,
        MTFS_TK_READY_BIT(worker->index));
    if (tk_wai_flg(worker->event_flag_id, MTFS_TK_START_BIT, TWF_ORW,
            &flags, MTFS_TK_WAIT_MS) < E_OK) {
        goto done;
    }

    worker->result = f_open(&worker->file, worker->path,
        FA_CREATE_ALWAYS | FA_WRITE);
    if (worker->result != FR_OK) {
        goto done;
    }
    worker->file_open = 1;
    for (iteration = 0U; iteration < MTFS_TK_ITERATIONS; ++iteration) {
        mtfs_tk_fill(worker->write_buffer, worker->seed, iteration);
        worker->transferred = 0U;
        worker->result = f_write(&worker->file, worker->write_buffer,
            sizeof(worker->write_buffer), &worker->transferred);
        if ((worker->result != FR_OK) ||
            (worker->transferred != sizeof(worker->write_buffer))) {
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
    for (iteration = 0U; iteration < MTFS_TK_ITERATIONS; ++iteration) {
        mtfs_tk_fill(worker->write_buffer, worker->seed, iteration);
        worker->transferred = 0U;
        worker->result = f_read(&worker->file, worker->read_buffer,
            sizeof(worker->read_buffer), &worker->transferred);
        if ((worker->result != FR_OK) ||
            (worker->transferred != sizeof(worker->read_buffer)) ||
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
    (void)tk_set_flg(worker->event_flag_id,
        MTFS_TK_DONE_BIT(worker->index));
    for (;;) {
        (void)tk_slp_tsk(TMO_FEVR);
    }
}

static int mtfs_tk_verify_persisted(
    mtfs_test_t *test, mtfs_tk_worker_t *worker)
{
    unsigned int iteration;
    FRESULT result;
    UINT transferred;
    int matches = 1;

    result = f_open(&worker->file, worker->path, FA_READ);
    if (!MTFS_TEST_CHECK(test, result == FR_OK,
            "open concurrent file after remount")) {
        return 0;
    }
    worker->file_open = 1;
    for (iteration = 0U; iteration < MTFS_TK_ITERATIONS; ++iteration) {
        mtfs_tk_fill(worker->write_buffer, worker->seed, iteration);
        transferred = 0U;
        result = f_read(&worker->file, worker->read_buffer,
            sizeof(worker->read_buffer), &transferred);
        if ((result != FR_OK) ||
            (transferred != sizeof(worker->read_buffer)) ||
            (memcmp(worker->write_buffer, worker->read_buffer,
                sizeof(worker->read_buffer)) != 0)) {
            matches = 0;
            break;
        }
    }
    (void)MTFS_TEST_CHECK(test, matches,
        "concurrent file persisted content matches");
    (void)MTFS_TEST_CHECK(test, f_close(&worker->file) == FR_OK,
        "close concurrent file after verification");
    worker->file_open = 0;
    return matches;
}

int mtfs_test_concurrent_microtkernel(mtfs_test_t *test,
    const char *volume_path, unsigned int outer_iteration)
{
    static const char *const paths[MTFS_TK_WORKERS] = {
        "0:TASKA.BIN", "0:TASKB.BIN"
    };
    static const unsigned int seeds[MTFS_TK_WORKERS] = {0x31U, 0xA7U};
    T_CFLG flag_config = {
        .flgatr = TA_TFIFO | TA_WMUL,
        .iflgptn = 0U
    };
    T_CTSK task_config = {
        .tskatr = TA_HLNG | TA_RNG3 | TA_USERBUF,
        .task = mtfs_tk_worker_task,
        .itskpri = 10,
        .stksz = MTFS_TK_WORKER_STACK_SIZE
    };
    ID task_ids[MTFS_TK_WORKERS] = {0, 0};
    int task_started[MTFS_TK_WORKERS] = {0, 0};
    ID event_flag_id = 0;
    UINT flags;
    FRESULT fat_result;
    unsigned int index;
    int mounted = 0;
    int result = 1;

    (void)outer_iteration;
    if ((test == NULL) || (volume_path == NULL)) {
        return result;
    }
    fat_result = f_mount(&concurrent_filesystem, volume_path, 1U);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK,
            "mount prepared volume for concurrent access")) {
        return result;
    }
    mounted = 1;
    event_flag_id = tk_cre_flg(&flag_config);
    if (!MTFS_TEST_CHECK(test, event_flag_id > 0,
            "create concurrent coordination event flag")) {
        goto cleanup;
    }

    (void)memset(workers, 0, sizeof(workers));
    for (index = 0U; index < MTFS_TK_WORKERS; ++index) {
        workers[index].index = index;
        workers[index].seed = seeds[index];
        workers[index].path = paths[index];
        workers[index].event_flag_id = event_flag_id;
        task_config.exinf = &workers[index];
        task_config.bufptr = worker_stacks[index];
        task_ids[index] = tk_cre_tsk(&task_config);
        if (task_ids[index] <= 0) {
            (void)MTFS_TEST_CHECK(test, 0, "create both worker tasks");
            goto cleanup_tasks;
        }
        if (tk_sta_tsk(task_ids[index], 0) < E_OK) {
            (void)MTFS_TEST_CHECK(test, 0, "start both worker tasks");
            goto cleanup_tasks;
        }
        task_started[index] = 1;
    }
    if (!MTFS_TEST_CHECK(test,
            tk_wai_flg(event_flag_id, MTFS_TK_READY_MASK, TWF_ANDW,
                &flags, MTFS_TK_WAIT_MS) >= E_OK,
            "wait until both worker tasks are ready")) {
        goto cleanup_tasks;
    }
    (void)tk_set_flg(event_flag_id, MTFS_TK_START_BIT);
    if (!MTFS_TEST_CHECK(test,
            tk_wai_flg(event_flag_id, MTFS_TK_DONE_MASK, TWF_ANDW,
                &flags, MTFS_TK_WAIT_MS) >= E_OK,
            "wait for both worker tasks to finish")) {
        goto cleanup_tasks;
    }
    for (index = 0U; index < MTFS_TK_WORKERS; ++index) {
        if (!MTFS_TEST_CHECK(test, workers[index].result == FR_OK,
                "worker write and immediate read-back succeeded")) {
            goto cleanup_tasks;
        }
    }

    if (!MTFS_TEST_CHECK(test, f_mount(NULL, volume_path, 0U) == FR_OK,
            "unmount after concurrent writes")) {
        goto cleanup_tasks;
    }
    mounted = 0;
    if (!MTFS_TEST_CHECK(test,
            f_mount(&concurrent_filesystem, volume_path, 1U) == FR_OK,
            "remount after concurrent writes")) {
        goto cleanup_tasks;
    }
    mounted = 1;
    for (index = 0U; index < MTFS_TK_WORKERS; ++index) {
        if (!mtfs_tk_verify_persisted(test, &workers[index])) {
            goto cleanup_tasks;
        }
    }
    for (index = 0U; index < MTFS_TK_WORKERS; ++index) {
        if (!MTFS_TEST_CHECK(test, f_unlink(paths[index]) == FR_OK,
                "remove concurrent test file")) {
            goto cleanup_tasks;
        }
    }
    result = 0;

cleanup_tasks:
    (void)tk_set_flg(event_flag_id, MTFS_TK_START_BIT);
    for (index = 0U; index < MTFS_TK_WORKERS; ++index) {
        if (task_ids[index] > 0) {
            if (task_started[index]) {
                (void)MTFS_TEST_CHECK(test,
                    tk_ter_tsk(task_ids[index]) >= E_OK,
                    "terminate worker task during cleanup");
            }
            if (workers[index].file_open) {
                (void)f_close(&workers[index].file);
                workers[index].file_open = 0;
            }
            (void)MTFS_TEST_CHECK(test, tk_del_tsk(task_ids[index]) >= E_OK,
                "delete worker task during cleanup");
        }
    }
    (void)MTFS_TEST_CHECK(test, tk_del_flg(event_flag_id) >= E_OK,
        "delete coordination event flag during cleanup");
    event_flag_id = 0;
cleanup:
    if (mounted) {
        if (result != 0) {
            for (index = 0U; index < MTFS_TK_WORKERS; ++index) {
                (void)f_unlink(paths[index]);
            }
        }
        (void)f_mount(NULL, volume_path, 0U);
    }
    return result;
}
