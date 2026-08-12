#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "test_fatfs_concurrent.h"

#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <string.h>

#include "ff.h"

#define MTFS_CONCURRENT_PATH_SIZE  (64U)
#define MTFS_CONCURRENT_CHUNK_SIZE (2048U)
#define MTFS_CONCURRENT_ITERATIONS (64U)
#define MTFS_CONCURRENT_WORKERS    (2U)

typedef struct mtfs_concurrent_gate
{
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned int ready;
    int start;
} mtfs_concurrent_gate_t;

typedef struct mtfs_concurrent_worker
{
    mtfs_concurrent_gate_t *gate;
    const char *path;
    unsigned int seed;
    FRESULT fat_result;
    UINT transferred;
    int thread_error;
} mtfs_concurrent_worker_t;

static int mtfs_concurrent_make_path(
    char *destination,
    size_t destination_size,
    const char *volume_path,
    const char *filename)
{
    size_t volume_length;
    size_t filename_length;

    if ((destination == NULL) || (volume_path == NULL) || (filename == NULL)) {
        return 0;
    }
    volume_length = strlen(volume_path);
    filename_length = strlen(filename);
    if ((volume_length >= destination_size) ||
        (filename_length >= (destination_size - volume_length))) {
        return 0;
    }
    memcpy(destination, volume_path, volume_length);
    memcpy(destination + volume_length, filename, filename_length + 1U);
    return 1;
}

static void mtfs_concurrent_fill(
    BYTE *buffer,
    unsigned int seed,
    unsigned int iteration)
{
    unsigned int i;

    for (i = 0U; i < MTFS_CONCURRENT_CHUNK_SIZE; ++i) {
        buffer[i] = (BYTE)((seed + (iteration * 17U) + (i * 37U)) & 0xFFU);
    }
}

static void *mtfs_concurrent_write_worker(void *argument)
{
    mtfs_concurrent_worker_t *worker = (mtfs_concurrent_worker_t *)argument;
    BYTE buffer[MTFS_CONCURRENT_CHUNK_SIZE];
    FIL file;
    unsigned int iteration;

    if (pthread_mutex_lock(&worker->gate->mutex) != 0) {
        worker->thread_error = 1;
        return NULL;
    }
    ++worker->gate->ready;
    (void)pthread_cond_broadcast(&worker->gate->condition);
    while (!worker->gate->start) {
        if (pthread_cond_wait(&worker->gate->condition, &worker->gate->mutex) != 0) {
            worker->thread_error = 1;
            (void)pthread_mutex_unlock(&worker->gate->mutex);
            return NULL;
        }
    }
    if (pthread_mutex_unlock(&worker->gate->mutex) != 0) {
        worker->thread_error = 1;
        return NULL;
    }

    worker->fat_result = f_open(&file, worker->path, FA_CREATE_ALWAYS | FA_WRITE);
    if (worker->fat_result != FR_OK) {
        return NULL;
    }
    for (iteration = 0U; iteration < MTFS_CONCURRENT_ITERATIONS; ++iteration) {
        mtfs_concurrent_fill(buffer, worker->seed, iteration);
        worker->transferred = 0U;
        worker->fat_result = f_write(&file, buffer, (UINT)sizeof(buffer),
            &worker->transferred);
        if ((worker->fat_result != FR_OK) ||
            (worker->transferred != (UINT)sizeof(buffer))) {
            break;
        }
        (void)sched_yield();
    }
    if (worker->fat_result == FR_OK) {
        worker->fat_result = f_sync(&file);
    }
    {
        FRESULT close_result = f_close(&file);
        if (worker->fat_result == FR_OK) {
            worker->fat_result = close_result;
        }
    }
    return NULL;
}

static int mtfs_concurrent_verify_file(
    mtfs_test_t *test,
    const char *path,
    unsigned int seed,
    const char *size_message,
    const char *content_message)
{
    BYTE expected[MTFS_CONCURRENT_CHUNK_SIZE];
    BYTE actual[MTFS_CONCURRENT_CHUNK_SIZE];
    FIL file;
    FRESULT fat_result;
    UINT transferred;
    unsigned int iteration;
    int result = 0;

    fat_result = f_open(&file, path, FA_READ);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK,
            "open concurrent output for read")) {
        return 0;
    }
    if (!MTFS_TEST_CHECK(test,
            f_size(&file) == (FSIZE_t)(MTFS_CONCURRENT_CHUNK_SIZE *
                MTFS_CONCURRENT_ITERATIONS),
            size_message)) {
        goto done;
    }
    for (iteration = 0U; iteration < MTFS_CONCURRENT_ITERATIONS; ++iteration) {
        mtfs_concurrent_fill(expected, seed, iteration);
        transferred = 0U;
        fat_result = f_read(&file, actual, (UINT)sizeof(actual), &transferred);
        if ((fat_result != FR_OK) || (transferred != (UINT)sizeof(actual)) ||
            (memcmp(expected, actual, sizeof(expected)) != 0)) {
            (void)MTFS_TEST_CHECK(test, 0, content_message);
            goto done;
        }
    }
    (void)MTFS_TEST_CHECK(test, 1, content_message);
    result = 1;

done:
    if (!MTFS_TEST_CHECK(test, f_close(&file) == FR_OK,
            "close verified concurrent output")) {
        result = 0;
    }
    return result;
}

int test_fatfs_concurrent(mtfs_test_t *test, const char *volume_path)
{
    static const char *const filenames[MTFS_CONCURRENT_WORKERS] = {
        "TASKA.BIN", "TASKB.BIN"
    };
    static const unsigned int seeds[MTFS_CONCURRENT_WORKERS] = {0x31U, 0xA7U};
    char paths[MTFS_CONCURRENT_WORKERS][MTFS_CONCURRENT_PATH_SIZE];
    pthread_t threads[MTFS_CONCURRENT_WORKERS];
    int thread_joined[MTFS_CONCURRENT_WORKERS] = {0, 0};
    mtfs_concurrent_worker_t workers[MTFS_CONCURRENT_WORKERS];
    mtfs_concurrent_gate_t gate;
    FATFS filesystem;
    FRESULT fat_result;
    unsigned int created_threads = 0U;
    unsigned int joined_threads = 0U;
    unsigned int i;
    int gate_mutex_initialized = 0;
    int gate_condition_initialized = 0;
    int mounted = 0;
    int result = 1;

    memset(&gate, 0, sizeof(gate));
    memset(workers, 0, sizeof(workers));
    for (i = 0U; i < MTFS_CONCURRENT_WORKERS; ++i) {
        if (!MTFS_TEST_CHECK(test,
                mtfs_concurrent_make_path(paths[i], sizeof(paths[i]), volume_path,
                    filenames[i]),
                "build concurrent path from the selected volume")) {
            return result;
        }
    }

    fat_result = f_mount(&filesystem, volume_path, 1U);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK,
            "mount prepared volume for concurrent access")) {
        goto cleanup;
    }
    mounted = 1;
    if (!MTFS_TEST_CHECK(test, pthread_mutex_init(&gate.mutex, NULL) == 0,
            "initialize concurrent start mutex")) {
        goto cleanup;
    }
    gate_mutex_initialized = 1;
    if (!MTFS_TEST_CHECK(test, pthread_cond_init(&gate.condition, NULL) == 0,
            "initialize concurrent start condition")) {
        goto cleanup;
    }
    gate_condition_initialized = 1;

    for (i = 0U; i < MTFS_CONCURRENT_WORKERS; ++i) {
        workers[i].gate = &gate;
        workers[i].path = paths[i];
        workers[i].seed = seeds[i];
        workers[i].fat_result = FR_INT_ERR;
        if (pthread_create(&threads[i], NULL, mtfs_concurrent_write_worker,
                &workers[i]) != 0) {
            (void)MTFS_TEST_CHECK(test, 0, "create concurrent writer thread");
            break;
        }
        ++created_threads;
    }

    if (pthread_mutex_lock(&gate.mutex) == 0) {
        while (gate.ready < created_threads) {
            if (pthread_cond_wait(&gate.condition, &gate.mutex) != 0) {
                (void)MTFS_TEST_CHECK(test, 0,
                    "wait for concurrent writers to become ready");
                break;
            }
        }
        gate.start = 1;
        (void)pthread_cond_broadcast(&gate.condition);
        (void)pthread_mutex_unlock(&gate.mutex);
    } else {
        (void)MTFS_TEST_CHECK(test, 0, "release concurrent writer threads");
    }
    for (i = 0U; i < created_threads; ++i) {
        if (pthread_join(threads[i], NULL) == 0) {
            thread_joined[i] = 1;
            ++joined_threads;
        } else {
            (void)MTFS_TEST_CHECK(test, 0, "join concurrent writer thread");
        }
    }
    if (!MTFS_TEST_CHECK(test,
            (created_threads == MTFS_CONCURRENT_WORKERS) &&
                (joined_threads == MTFS_CONCURRENT_WORKERS),
            "run and join both concurrent writers")) {
        goto cleanup;
    }
    for (i = 0U; i < MTFS_CONCURRENT_WORKERS; ++i) {
        if (!MTFS_TEST_CHECK(test,
                !workers[i].thread_error && (workers[i].fat_result == FR_OK),
                "concurrent writer completed all writes and sync")) {
            goto cleanup;
        }
    }

    fat_result = f_mount(NULL, volume_path, 0U);
    mounted = 0;
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK,
            "unmount after concurrent writes")) {
        goto cleanup;
    }
    fat_result = f_mount(&filesystem, volume_path, 1U);
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK,
            "remount after concurrent writes")) {
        goto cleanup;
    }
    mounted = 1;

    if (!mtfs_concurrent_verify_file(test, paths[0], seeds[0],
            "TASKA.BIN persisted size matches",
            "TASKA.BIN persisted content matches") ||
        !mtfs_concurrent_verify_file(test, paths[1], seeds[1],
            "TASKB.BIN persisted size matches",
            "TASKB.BIN persisted content matches")) {
        goto cleanup;
    }
    for (i = 0U; i < MTFS_CONCURRENT_WORKERS; ++i) {
        if (!MTFS_TEST_CHECK(test, f_unlink(paths[i]) == FR_OK,
                "remove dedicated concurrent test file")) {
            goto cleanup;
        }
    }
    fat_result = f_mount(NULL, volume_path, 0U);
    mounted = 0;
    if (!MTFS_TEST_CHECK(test, fat_result == FR_OK,
            "final concurrent-test unmount")) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (created_threads > joined_threads) {
        if (gate_mutex_initialized && gate_condition_initialized &&
            (pthread_mutex_lock(&gate.mutex) == 0)) {
            gate.start = 1;
            (void)pthread_cond_broadcast(&gate.condition);
            (void)pthread_mutex_unlock(&gate.mutex);
        }
        for (i = 0U; i < created_threads; ++i) {
            if (!thread_joined[i]) {
                (void)pthread_join(threads[i], NULL);
            }
        }
    }
    if (gate_condition_initialized) {
        (void)pthread_cond_destroy(&gate.condition);
    }
    if (gate_mutex_initialized) {
        (void)pthread_mutex_destroy(&gate.mutex);
    }
    if (mounted) {
        for (i = 0U; i < MTFS_CONCURRENT_WORKERS; ++i) {
            (void)f_unlink(paths[i]);
        }
        (void)f_mount(NULL, volume_path, 0U);
    }
    return result;
}
