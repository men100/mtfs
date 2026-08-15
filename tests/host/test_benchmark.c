#include "test_benchmark.h"

#include <stdint.h>
#include <string.h>

#include "ff.h"
#include "mtfs_benchmark.h"
#include "mtfs_block_registry.h"

typedef struct benchmark_clock
{
    uint64_t value;
    uint64_t step;
} benchmark_clock_t;

typedef struct benchmark_log
{
    unsigned int lines;
    unsigned int suite_passes;
    unsigned int cleanup_lines;
    unsigned int existing_file_errors;
} benchmark_log_t;

typedef struct benchmark_proxy
{
    mtfs_block_device_t *backing;
    uint32_t read_calls;
    uint32_t write_calls;
    uint32_t sync_calls;
    uint32_t fail_read_call;
    uint32_t fail_write_call;
    uint32_t fail_sync_call;
} benchmark_proxy_t;

static uint64_t benchmark_fake_clock(void *opaque)
{
    benchmark_clock_t *clock = (benchmark_clock_t *)opaque;
    uint64_t result = clock->value;
    clock->value += clock->step;
    return result;
}

static void benchmark_capture_log(void *opaque, const char *line)
{
    benchmark_log_t *log = (benchmark_log_t *)opaque;
    ++log->lines;
    if (strstr(line, "SUITE END status=PASS") != NULL) {
        ++log->suite_passes;
    }
    if (strstr(line, "cleanup file_removed=yes") != NULL) {
        ++log->cleanup_lines;
    }
    if (strstr(line, "stage=temp_file_exists") != NULL) {
        ++log->existing_file_errors;
    }
}

static mtfs_error_t proxy_initialize(void *opaque)
{
    benchmark_proxy_t *proxy = (benchmark_proxy_t *)opaque;
    return mtfs_block_initialize(proxy->backing);
}

static mtfs_error_t proxy_status(void *opaque, mtfs_block_status_t *status)
{
    benchmark_proxy_t *proxy = (benchmark_proxy_t *)opaque;
    return mtfs_block_status(proxy->backing, status);
}

static mtfs_error_t proxy_read(
    void *opaque, void *buffer, mtfs_lba_t lba, uint32_t count)
{
    benchmark_proxy_t *proxy = (benchmark_proxy_t *)opaque;
    ++proxy->read_calls;
    if ((proxy->fail_read_call != 0U) &&
        (proxy->read_calls == proxy->fail_read_call)) {
        return MTFS_ERROR_IO;
    }
    return mtfs_block_read(proxy->backing, buffer, lba, count);
}

static mtfs_error_t proxy_write(
    void *opaque, const void *buffer, mtfs_lba_t lba, uint32_t count)
{
    benchmark_proxy_t *proxy = (benchmark_proxy_t *)opaque;
    ++proxy->write_calls;
    if ((proxy->fail_write_call != 0U) &&
        (proxy->write_calls == proxy->fail_write_call)) {
        return MTFS_ERROR_IO;
    }
    return mtfs_block_write(proxy->backing, buffer, lba, count);
}

static mtfs_error_t proxy_sync(void *opaque)
{
    benchmark_proxy_t *proxy = (benchmark_proxy_t *)opaque;
    ++proxy->sync_calls;
    if ((proxy->fail_sync_call != 0U) &&
        (proxy->sync_calls == proxy->fail_sync_call)) {
        return MTFS_ERROR_IO;
    }
    return mtfs_block_sync(proxy->backing);
}

static mtfs_error_t proxy_geometry(
    void *opaque, mtfs_block_geometry_t *geometry)
{
    benchmark_proxy_t *proxy = (benchmark_proxy_t *)opaque;
    return mtfs_block_get_geometry(proxy->backing, geometry);
}

static const mtfs_block_device_ops_t proxy_ops = {
    proxy_initialize,
    proxy_status,
    proxy_read,
    proxy_write,
    proxy_sync,
    proxy_geometry,
    NULL
};

static void benchmark_config_init(mtfs_benchmark_config_t *config,
    mtfs_block_device_t *device, const char *volume_path,
    benchmark_clock_t *clock, benchmark_log_t *log, uint8_t *buffer)
{
    memset(config, 0, sizeof(*config));
    config->device = device;
    config->volume_path = volume_path;
    config->board_name = "host-fake";
    config->build_name = "host-test";
    config->clock_us = benchmark_fake_clock;
    config->clock_context = clock;
    config->log = benchmark_capture_log;
    config->log_context = log;
    config->buffer = buffer;
    config->buffer_size = MTFS_BENCHMARK_BUFFER_BYTES;
}

static int benchmark_temp_file_absent(
    mtfs_test_t *test, const char *volume_path)
{
    FATFS filesystem;
    FILINFO info;
    FRESULT result;
    char path[32];
    size_t length = strlen(volume_path);
    int passed;

    if (length + sizeof("/" MTFS_BENCHMARK_FILE_NAME) > sizeof(path)) {
        return MTFS_TEST_CHECK(test, 0, "build benchmark temp path");
    }
    memcpy(path, volume_path, length);
    path[length++] = '/';
    memcpy(path + length, MTFS_BENCHMARK_FILE_NAME,
        sizeof(MTFS_BENCHMARK_FILE_NAME));
    result = f_mount(&filesystem, volume_path, 1U);
    passed = MTFS_TEST_CHECK(test, result == FR_OK,
        "mount to inspect benchmark cleanup");
    if (passed) {
        passed = MTFS_TEST_CHECK(test, f_stat(path, &info) == FR_NO_FILE,
            "benchmark cleanup leaves no temporary file");
        (void)MTFS_TEST_CHECK(test, f_mount(NULL, volume_path, 0U) == FR_OK,
            "unmount after benchmark cleanup inspection");
    }
    return passed;
}

static int benchmark_run_failure_case(mtfs_test_t *test,
    mtfs_block_device_t *backing, const char *volume_path,
    uint32_t fail_write_call, uint32_t fail_sync_call,
    const char *failure_message)
{
    static uint8_t buffer[MTFS_BENCHMARK_BUFFER_BYTES];
    benchmark_proxy_t proxy_context;
    mtfs_block_device_t proxy_device;
    mtfs_benchmark_config_t config;
    benchmark_clock_t clock = {0U, 100U};
    benchmark_log_t log = {0U, 0U, 0U, 0U};
    int result;

    memset(&proxy_context, 0, sizeof(proxy_context));
    proxy_context.backing = backing;
    proxy_context.fail_write_call = fail_write_call;
    proxy_context.fail_sync_call = fail_sync_call;
    proxy_device.ops = &proxy_ops;
    proxy_device.context = &proxy_context;
    proxy_device.capabilities = backing->capabilities;

    if (!MTFS_TEST_CHECK(test,
            mtfs_block_registry_unregister(0U) == MTFS_OK,
            "temporarily unregister backing device")) {
        return 0;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_block_registry_register(0U, &proxy_device) == MTFS_OK,
            "register fault-injection proxy")) {
        (void)mtfs_block_registry_register(0U, backing);
        return 0;
    }
    benchmark_config_init(&config, &proxy_device, volume_path,
        &clock, &log, buffer);
    result = mtfs_benchmark_run(&config, MTFS_BENCHMARK_PROFILE_SMOKE);
    (void)mtfs_block_registry_unregister(0U);
    (void)MTFS_TEST_CHECK(test,
        mtfs_block_registry_register(0U, backing) == MTFS_OK,
        "restore backing device after fault injection");
    if (!MTFS_TEST_CHECK(test, result != 0, failure_message)) {
        return 0;
    }
    return benchmark_temp_file_absent(test, volume_path);
}

int test_benchmark(mtfs_test_t *test, mtfs_block_device_t *device,
    const char *volume_path)
{
    static uint8_t buffer[MTFS_BENCHMARK_BUFFER_BYTES];
    mtfs_benchmark_config_t config;
    mtfs_benchmark_rate_t rate;
    mtfs_benchmark_latency_t latency;
    benchmark_clock_t clock = {0U, 100U};
    benchmark_log_t log = {0U, 0U, 0U, 0U};
    uint64_t raw_bytes;
    uint64_t file_bytes;
    uint64_t sync_bytes;
    uint64_t mismatch = 0U;
    FATFS filesystem;
    FIL file;
    char existing_path[32];
    size_t volume_length = strlen(volume_path);

    mtfs_benchmark_rate_calculate(1024U, 4U, 1000000U, &rate);
    if (!MTFS_TEST_CHECK(test,
            (rate.throughput_tenths_kib_s == 10U) &&
                (rate.iops_tenths == 40U),
            "calculate throughput and IOPS with integer tenths")) {
        return 1;
    }
    mtfs_benchmark_rate_calculate(UINT64_MAX, UINT64_MAX, UINT64_MAX, &rate);
    if (!MTFS_TEST_CHECK(test,
            (rate.throughput_tenths_kib_s == 0U) &&
                (rate.iops_tenths != 0U),
            "large 64-bit counters do not overflow rate calculation")) {
        return 1;
    }
    mtfs_benchmark_rate_calculate(1024U, 1U, 0U, &rate);
    if (!MTFS_TEST_CHECK(test,
            (rate.throughput_tenths_kib_s == 0U) &&
                (rate.iops_tenths == 0U),
            "zero elapsed time is guarded")) {
        return 1;
    }
    mtfs_benchmark_latency_reset(&latency);
    mtfs_benchmark_latency_add(&latency, 30U);
    mtfs_benchmark_latency_add(&latency, 10U);
    mtfs_benchmark_latency_add(&latency, 50U);
    if (!MTFS_TEST_CHECK(test,
            (latency.minimum_us == 10U) &&
                (latency.total_us == 90U) &&
                (latency.maximum_us == 50U) && (latency.samples == 3U),
            "accumulate latency minimum average input and maximum")) {
        return 1;
    }
    mtfs_benchmark_pattern_fill(buffer, 1024U, UINT64_C(0x100000000));
    if (!MTFS_TEST_CHECK(test,
            mtfs_benchmark_pattern_verify(buffer, 1024U,
                UINT64_C(0x100000000), &mismatch),
            "verify deterministic benchmark pattern at 64-bit offset")) {
        return 1;
    }
    buffer[777] ^= 1U;
    if (!MTFS_TEST_CHECK(test,
            !mtfs_benchmark_pattern_verify(buffer, 1024U,
                UINT64_C(0x100000000), &mismatch) &&
                (mismatch == UINT64_C(0x100000309)),
            "detect and locate pattern mismatch")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_benchmark_profile_sizes(MTFS_BENCHMARK_PROFILE_SMOKE,
                &raw_bytes, &file_bytes, &sync_bytes) &&
                (raw_bytes == 65536U) && (file_bytes == 65536U) &&
                (sync_bytes == 32768U),
            "smoke profile has fixed portable sizes")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_benchmark_profile_sizes(MTFS_BENCHMARK_PROFILE_NORMAL,
                &raw_bytes, &file_bytes, &sync_bytes) &&
                (raw_bytes == 1048576U) && (file_bytes == 1048576U) &&
                (sync_bytes == 65536U),
            "normal profile has fixed one MiB baseline size")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            !mtfs_benchmark_has_sufficient_space(65535U, 65536U) &&
                mtfs_benchmark_has_sufficient_space(65536U, 65536U),
            "reject insufficient free space at exact boundary")) {
        return 1;
    }

    benchmark_config_init(&config, device, volume_path, &clock, &log, buffer);
    if (!MTFS_TEST_CHECK(test,
            mtfs_benchmark_run(&config, MTFS_BENCHMARK_PROFILE_SMOKE) == 0,
            "run complete smoke benchmark with fake clock")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            (log.suite_passes == 1U) && (log.cleanup_lines == 6U),
            "smoke benchmark reports PASS and cleans every owned file")) {
        return 1;
    }
    if (!benchmark_temp_file_absent(test, volume_path)) {
        return 1;
    }

    if (volume_length + sizeof("/" MTFS_BENCHMARK_FILE_NAME) >
        sizeof(existing_path)) {
        return MTFS_TEST_CHECK(test, 0, "build pre-existing benchmark path");
    }
    memcpy(existing_path, volume_path, volume_length);
    existing_path[volume_length++] = '/';
    memcpy(existing_path + volume_length, MTFS_BENCHMARK_FILE_NAME,
        sizeof(MTFS_BENCHMARK_FILE_NAME));
    if (!MTFS_TEST_CHECK(test,
            f_mount(&filesystem, volume_path, 1U) == FR_OK &&
                f_open(&file, existing_path,
                    FA_WRITE | FA_CREATE_NEW) == FR_OK &&
                f_close(&file) == FR_OK &&
                f_mount(NULL, volume_path, 0U) == FR_OK,
            "create sentinel benchmark file without overwriting data")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            mtfs_benchmark_run(&config, MTFS_BENCHMARK_PROFILE_SMOKE) != 0 &&
                log.existing_file_errors == 1U,
            "abort when benchmark filename already exists")) {
        return 1;
    }
    if (!MTFS_TEST_CHECK(test,
            f_mount(&filesystem, volume_path, 1U) == FR_OK &&
                f_stat(existing_path, &(FILINFO){0}) == FR_OK &&
                f_unlink(existing_path) == FR_OK &&
                f_mount(NULL, volume_path, 0U) == FR_OK,
            "preserve pre-existing file until explicit test cleanup")) {
        return 1;
    }

    {
        benchmark_proxy_t proxy_context;
        mtfs_block_device_t proxy_device;
        memset(&proxy_context, 0, sizeof(proxy_context));
        proxy_context.backing = device;
        proxy_context.fail_read_call = 1U;
        proxy_device.ops = &proxy_ops;
        proxy_device.context = &proxy_context;
        proxy_device.capabilities = device->capabilities;
        benchmark_config_init(&config, &proxy_device, volume_path,
            &clock, &log, buffer);
        if (!MTFS_TEST_CHECK(test,
                mtfs_benchmark_run(&config,
                    MTFS_BENCHMARK_PROFILE_SMOKE) != 0,
                "propagate raw read failure")) {
            return 1;
        }
    }
    if (!benchmark_run_failure_case(test, device, volume_path,
            1U, 0U, "propagate FatFs write failure and enter cleanup")) {
        return 1;
    }
    if (!benchmark_run_failure_case(test, device, volume_path,
            0U, 1U, "propagate FatFs sync failure and enter cleanup")) {
        return 1;
    }
    return 0;
}
