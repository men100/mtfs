#include "mtfs_sentinel_recorder.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"

#if MTFS_ENABLE_STORAGE_SENTINEL
typedef struct mtfs_sentinel_csv_writer
{
    char *buffer;
    size_t capacity;
    size_t used;
    int overflow;
} mtfs_sentinel_csv_writer_t;

static void csv_writer_init(mtfs_sentinel_csv_writer_t *writer,
    char *buffer, size_t capacity)
{
    writer->buffer = buffer;
    writer->capacity = capacity;
    writer->used = 0U;
    writer->overflow = 0;
    buffer[0] = '\0';
}

static void csv_append_char(mtfs_sentinel_csv_writer_t *writer, char value)
{
    if (writer->overflow) return;
    if (writer->used + 1U >= writer->capacity) {
        writer->overflow = 1;
        return;
    }
    writer->buffer[writer->used++] = value;
    writer->buffer[writer->used] = '\0';
}

static void csv_append_text(mtfs_sentinel_csv_writer_t *writer,
    const char *text)
{
    while (*text != '\0' && !writer->overflow)
        csv_append_char(writer, *text++);
}

static void csv_append_u64(mtfs_sentinel_csv_writer_t *writer,
    uint64_t value)
{
    static const uint64_t decimal_place[] = {
        UINT64_C(10000000000000000000), UINT64_C(1000000000000000000),
        UINT64_C(100000000000000000), UINT64_C(10000000000000000),
        UINT64_C(1000000000000000), UINT64_C(100000000000000),
        UINT64_C(10000000000000), UINT64_C(1000000000000),
        UINT64_C(100000000000), UINT64_C(10000000000),
        UINT64_C(1000000000), UINT64_C(100000000), UINT64_C(10000000),
        UINT64_C(1000000), UINT64_C(100000), UINT64_C(10000),
        UINT64_C(1000), UINT64_C(100), UINT64_C(10), UINT64_C(1)
    };
    size_t place;
    int started = 0;
    for (place = 0U; place < sizeof(decimal_place) /
            sizeof(decimal_place[0]); ++place) {
        uint32_t digit = 0U;
        while (value >= decimal_place[place]) {
            value -= decimal_place[place];
            ++digit;
        }
        if (digit != 0U || started || decimal_place[place] == UINT64_C(1)) {
            csv_append_char(writer, (char)('0' + digit));
            started = 1;
        }
    }
}

static void csv_append_u32(mtfs_sentinel_csv_writer_t *writer,
    uint32_t value)
{
    csv_append_u64(writer, value);
}

static void csv_append_u64_field(mtfs_sentinel_csv_writer_t *writer,
    uint64_t value)
{
    csv_append_char(writer, ',');
    csv_append_u64(writer, value);
}

static void csv_append_u32_field(mtfs_sentinel_csv_writer_t *writer,
    uint32_t value)
{
    csv_append_char(writer, ',');
    csv_append_u32(writer, value);
}

const char *mtfs_sentinel_recorder_csv_header(void)
{
    return "mtfs-sentinel-csv-v1,version,size,target,transport,timestamp_us,"
        "interval_us,media_generation,validity,flags,samples,label,marker,"
        "read_calls,read_sectors,read_ok,read_fail,read_timing,read_invalid,read_total_us,read_avg_us,"
        "write_calls,write_sectors,write_ok,write_fail,write_timing,write_invalid,write_total_us,write_avg_us,"
        "sync_calls,sync_sectors,sync_ok,sync_fail,sync_timing,sync_invalid,sync_total_us,sync_avg_us,"
        "io_error,not_ready,no_media,timeout,insert,remove,media_error,histogram_r_w_s";
}

mtfs_error_t mtfs_sentinel_recorder_format_csv(char *buffer, size_t capacity,
    const mtfs_sentinel_feature_v1_t *f, const char *label, uint32_t marker)
{
    mtfs_sentinel_csv_writer_t writer;
    uint32_t op, bucket;
    if (buffer == NULL || capacity == 0U || f == NULL || label == NULL ||
        f->version != MTFS_SENTINEL_SCHEMA_VERSION) return MTFS_ERROR_INVALID_ARGUMENT;
    csv_writer_init(&writer, buffer, capacity);
    csv_append_text(&writer, "mtfs-sentinel-csv-v1");
    csv_append_u32_field(&writer, f->version);
    csv_append_u32_field(&writer, f->struct_size);
    csv_append_u32_field(&writer, f->target_id);
    csv_append_u32_field(&writer, f->transport_id);
    csv_append_u64_field(&writer, f->timestamp_us);
    csv_append_u64_field(&writer, f->observation_interval_us);
    csv_append_u32_field(&writer, f->media_generation);
    csv_append_u64_field(&writer, f->validity_mask);
    csv_append_u32_field(&writer, f->flags);
    csv_append_u32_field(&writer, f->sample_count);
    csv_append_char(&writer, ',');
    csv_append_text(&writer, label);
    csv_append_u32_field(&writer, marker);
    for (op = 0U; op < MTFS_SENTINEL_OPERATION_COUNT; ++op) {
        const mtfs_sentinel_operation_feature_t *o = &f->operation[op];
        csv_append_u64_field(&writer, o->calls);
        csv_append_u64_field(&writer, o->sectors_requested);
        csv_append_u64_field(&writer, o->successes);
        csv_append_u64_field(&writer, o->failures);
        csv_append_u64_field(&writer, o->timing_samples);
        csv_append_u64_field(&writer, o->timing_invalid);
        csv_append_u64_field(&writer, o->total_latency_us);
        csv_append_u64_field(&writer, o->average_latency_us);
    }
    csv_append_u64_field(&writer, f->io_errors);
    csv_append_u64_field(&writer, f->not_ready_errors);
    csv_append_u64_field(&writer, f->no_media_errors);
    csv_append_u64_field(&writer, f->timeout_errors);
    csv_append_u32_field(&writer, f->inserted_events);
    csv_append_u32_field(&writer, f->removed_events);
    csv_append_u32_field(&writer, f->media_error_events);
    for (op = 0U; op < MTFS_SENTINEL_OPERATION_COUNT; ++op) {
        for (bucket = 0U; bucket < MTFS_SENTINEL_HISTOGRAM_BUCKETS; ++bucket) {
            csv_append_char(&writer,
                (op == 0U && bucket == 0U) ? ',' : ':');
            csv_append_u64(&writer,
                f->operation[op].latency_histogram[bucket]);
        }
    }
    return writer.overflow ? MTFS_ERROR_BUFFER_TOO_SMALL : MTFS_OK;
}
#endif

mtfs_error_t mtfs_sentinel_recorder_workload(const char *volume,
    uint32_t marker, void *buffer, uint32_t size)
{
    FIL file;
    char path[32];
    UINT transferred = 0U;
    FRESULT result;
    mtfs_error_t error = MTFS_ERROR_IO;
    if (volume == NULL || buffer == NULL || size == 0U) return MTFS_ERROR_INVALID_ARGUMENT;
    if (snprintf(path, sizeof(path), "%s/MTFS%04" PRIX32 ".TMP",
            volume, marker & UINT32_C(0xffff)) >= (int)sizeof(path))
        return MTFS_ERROR_BUFFER_TOO_SMALL;
    (void)memset(buffer, (int)(marker & 0xffU), size);
    result = f_open(&file, path, FA_CREATE_NEW | FA_READ | FA_WRITE);
    if (result != FR_OK) return result == FR_EXIST ? MTFS_ERROR_ALREADY_EXISTS : MTFS_ERROR_IO;
    if (f_write(&file, buffer, size, &transferred) == FR_OK && transferred == size &&
        f_sync(&file) == FR_OK && f_lseek(&file, 0U) == FR_OK &&
        f_read(&file, buffer, size, &transferred) == FR_OK && transferred == size)
        error = MTFS_OK;
    if (f_close(&file) != FR_OK) error = MTFS_ERROR_IO;
    if (f_unlink(path) != FR_OK) error = MTFS_ERROR_IO;
    return error;
}
