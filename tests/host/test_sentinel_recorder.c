#include <stdint.h>
#include <string.h>

#include "ff.h"
#include "mtfs_sentinel_recorder.h"

static char opened_path[32];
static char removed_path[32];
static FRESULT open_result = FR_OK;
static uint32_t cleanup_calls;

static void cleanup(void *context)
{
    (void)context;
    ++cleanup_calls;
}

static size_t count_character(const char *text, char character)
{
    size_t count = 0U;
    while (*text != '\0') {
        if (*text++ == character) ++count;
    }
    return count;
}

FRESULT f_open(FIL *file, const TCHAR *path, BYTE mode)
{
    (void)file;
    if (mode != (FA_CREATE_NEW | FA_READ | FA_WRITE)) return FR_INVALID_PARAMETER;
    (void)strncpy(opened_path, path, sizeof(opened_path) - 1U);
    opened_path[sizeof(opened_path) - 1U] = '\0';
    return open_result;
}
FRESULT f_close(FIL *file) { (void)file; return FR_OK; }
FRESULT f_read(FIL *file, void *buffer, UINT requested, UINT *read_size)
{
    (void)file; (void)buffer;
    *read_size = requested;
    return FR_OK;
}
FRESULT f_write(FIL *file, const void *buffer, UINT requested, UINT *write_size)
{
    (void)file; (void)buffer;
    *write_size = requested;
    return FR_OK;
}
FRESULT f_lseek(FIL *file, FSIZE_t offset)
{
    (void)file; (void)offset;
    return FR_OK;
}
FRESULT f_sync(FIL *file) { (void)file; return FR_OK; }
FRESULT f_unlink(const TCHAR *path)
{
    (void)strncpy(removed_path, path, sizeof(removed_path) - 1U);
    removed_path[sizeof(removed_path) - 1U] = '\0';
    return FR_OK;
}

int main(void)
{
    mtfs_sentinel_feature_v1_t frame;
    char line[4096];
    char small[8];
    uint8_t workload[16];
    uint32_t operation;
    uint32_t bucket;
    (void)memset(&frame, 0, sizeof(frame));
    frame.version = MTFS_SENTINEL_SCHEMA_VERSION;
    frame.struct_size = (uint16_t)sizeof(frame);
    frame.target_id = 17U;
    frame.transport_id = 34U;
    frame.timestamp_us = UINT64_C(4294967297);
    frame.observation_interval_us = UINT64_MAX;
    frame.media_generation = 5U;
    frame.validity_mask = UINT64_C(9223372036854775808);
    frame.flags = 6U;
    frame.sample_count = 7U;
    for (operation = 0U; operation < MTFS_SENTINEL_OPERATION_COUNT;
         ++operation) {
        frame.operation[operation].calls = UINT64_MAX;
        frame.operation[operation].sectors_requested = UINT64_C(4294967296);
        frame.operation[operation].successes = UINT64_MAX;
        frame.operation[operation].failures = UINT64_MAX;
        frame.operation[operation].timing_samples = UINT64_MAX;
        frame.operation[operation].timing_invalid = UINT64_MAX;
        frame.operation[operation].average_latency_us = UINT64_MAX;
        for (bucket = 0U; bucket < MTFS_SENTINEL_HISTOGRAM_BUCKETS; ++bucket)
            frame.operation[operation].latency_histogram[bucket] = UINT64_MAX;
    }
    frame.io_errors = UINT64_MAX;
    frame.not_ready_errors = UINT64_MAX;
    frame.no_media_errors = UINT64_MAX;
    frame.timeout_errors = UINT64_MAX;
    frame.inserted_events = 9U;
    frame.removed_events = 10U;
    frame.media_error_events = 11U;
    frame.transport.validity_mask = MTFS_SENTINEL_TRANSPORT_VALID_ALL;
    frame.transport.flags = MTFS_SENTINEL_TRANSPORT_FLAG_COUNTERS_SATURATE;
    frame.transport.reset_epoch = 12U;
    frame.transport.transport_errors = UINT64_MAX;
    frame.transport.transfer_timeouts = UINT64_C(13);
    frame.transport.ready_timeouts = UINT64_C(14);
    frame.transport.aborts = UINT64_C(15);
    frame.transport.clock_errors = UINT64_C(16);
    if (mtfs_sentinel_recorder_format_csv(line, sizeof(line), &frame,
            "controlled", 8U) != MTFS_OK)
        return 1;
    if (strstr(mtfs_sentinel_recorder_csv_header(),
            ",sync_calls,sync_sectors,sync_ok,sync_fail,") == NULL ||
        strstr(mtfs_sentinel_recorder_csv_header(),
            ",transport_validity,transport_flags,transport_reset_epoch,"
            "transport_errors,transfer_timeouts,ready_timeouts,aborts,"
            "clock_errors,") == NULL ||
        count_character(mtfs_sentinel_recorder_csv_header(), ',') !=
            count_character(line, ','))
        return 1;
    if (strstr(line,
            ",17,34,4294967297,18446744073709551615,5,"
            "9223372036854775808,6,7,controlled,8,") == NULL ||
        strstr(line,
            ",18446744073709551615,4294967296,"
            "18446744073709551615") == NULL ||
        strstr(line,
            ",31,1,12,18446744073709551615,13,14,15,16,") == NULL)
        return 1;
    if (mtfs_sentinel_recorder_format_csv(small, sizeof(small), &frame,
            "controlled", 8U) != MTFS_ERROR_BUFFER_TOO_SMALL ||
        small[sizeof(small) - 1U] != '\0')
        return 1;
    frame.version = 0U;
    if (mtfs_sentinel_recorder_format_csv(line, sizeof(line), &frame,
            "controlled", 8U) != MTFS_ERROR_INVALID_ARGUMENT)
        return 1;
    if (mtfs_sentinel_recorder_workload("0:", UINT32_C(0x12345),
            workload, sizeof(workload)) != MTFS_OK ||
        strcmp(opened_path, "0:/MTFS2345.TMP") != 0 ||
        strcmp(removed_path, opened_path) != 0 ||
        workload[0] != UINT8_C(0x45) ||
        workload[sizeof(workload) - 1U] != UINT8_C(0x45))
        return 1;
    open_result = FR_NOT_READY;
    if (mtfs_sentinel_recorder_workload_ex("0:", 1U, workload,
            sizeof(workload), cleanup, NULL) != MTFS_ERROR_IO ||
        cleanup_calls != 1U)
        return 1;
    return 0;
}
