#include "mtfs_benchmark.h"

#include <limits.h>
#include <string.h>

#define MTFS_BENCHMARK_LINE_BYTES (256U)
#define MTFS_BENCHMARK_REQUEST_COUNT (3U)

typedef struct mtfs_benchmark_line
{
    char text[MTFS_BENCHMARK_LINE_BYTES];
    size_t length;
} mtfs_benchmark_line_t;

static const uint32_t mtfs_benchmark_request_bytes[MTFS_BENCHMARK_REQUEST_COUNT] = {
    512U, 4096U, 32768U
};

static uint64_t mtfs_benchmark_min_u64(uint64_t left, uint64_t right)
{
    return left < right ? left : right;
}

static void mtfs_benchmark_line_begin(
    mtfs_benchmark_line_t *line, const char *text)
{
    line->length = 0U;
    line->text[0] = '\0';
    while ((*text != '\0') &&
        (line->length + 1U < sizeof(line->text))) {
        line->text[line->length++] = *text++;
    }
    line->text[line->length] = '\0';
}

static void mtfs_benchmark_line_text(
    mtfs_benchmark_line_t *line, const char *text)
{
    while ((*text != '\0') &&
        (line->length + 1U < sizeof(line->text))) {
        line->text[line->length++] = *text++;
    }
    line->text[line->length] = '\0';
}

static void mtfs_benchmark_line_u64(
    mtfs_benchmark_line_t *line, uint64_t value)
{
    char digits[21];
    size_t count = 0U;
    do {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while ((value != 0U) && (count < sizeof(digits)));
    while (count != 0U) {
        char digit[2];
        digit[0] = digits[--count];
        digit[1] = '\0';
        mtfs_benchmark_line_text(line, digit);
    }
}

static void mtfs_benchmark_line_s32(
    mtfs_benchmark_line_t *line, int32_t value)
{
    if (value < 0) {
        mtfs_benchmark_line_text(line, "-");
        mtfs_benchmark_line_u64(line,
            (uint64_t)(-(int64_t)value));
    } else {
        mtfs_benchmark_line_u64(line, (uint64_t)value);
    }
}

static void mtfs_benchmark_line_fixed1(
    mtfs_benchmark_line_t *line, uint64_t tenths)
{
    mtfs_benchmark_line_u64(line, tenths / 10U);
    mtfs_benchmark_line_text(line, ".");
    mtfs_benchmark_line_u64(line, tenths % 10U);
}

static void mtfs_benchmark_emit(
    const mtfs_benchmark_config_t *config, mtfs_benchmark_line_t *line)
{
    config->log(config->log_context, line->text);
}

static uint64_t mtfs_benchmark_mul_div(
    uint64_t value, uint64_t multiplier, uint64_t divisor)
{
    uint64_t quotient;
    uint64_t remainder;

    if (divisor == 0U) {
        return 0U;
    }
    quotient = value / divisor;
    remainder = value % divisor;
    if (quotient > UINT64_MAX / multiplier) {
        return UINT64_MAX;
    }
    return quotient * multiplier + (remainder * multiplier) / divisor;
}

const char *mtfs_benchmark_profile_name(mtfs_benchmark_profile_t profile)
{
    if (profile == MTFS_BENCHMARK_PROFILE_SMOKE) {
        return "smoke";
    }
    if (profile == MTFS_BENCHMARK_PROFILE_NORMAL) {
        return "normal";
    }
    return "invalid";
}

int mtfs_benchmark_profile_sizes(mtfs_benchmark_profile_t profile,
    uint64_t *raw_total_bytes, uint64_t *file_total_bytes,
    uint64_t *sync_total_bytes)
{
    if ((raw_total_bytes == NULL) || (file_total_bytes == NULL) ||
        (sync_total_bytes == NULL)) {
        return 0;
    }
    if (profile == MTFS_BENCHMARK_PROFILE_SMOKE) {
        *raw_total_bytes = 64U * 1024U;
        *file_total_bytes = 64U * 1024U;
        *sync_total_bytes = 32U * 1024U;
        return 1;
    }
    if (profile == MTFS_BENCHMARK_PROFILE_NORMAL) {
        *raw_total_bytes = 1024U * 1024U;
        *file_total_bytes = 1024U * 1024U;
        *sync_total_bytes = 64U * 1024U;
        return 1;
    }
    return 0;
}

int mtfs_benchmark_has_sufficient_space(
    uint64_t free_bytes, uint64_t required_bytes)
{
    return free_bytes >= required_bytes;
}

void mtfs_benchmark_rate_calculate(uint64_t bytes, uint64_t requests,
    uint64_t elapsed_us, mtfs_benchmark_rate_t *rate)
{
    if (rate == NULL) {
        return;
    }
    rate->throughput_tenths_kib_s = 0U;
    rate->iops_tenths = 0U;
    if (elapsed_us == 0U) {
        return;
    }
    if (elapsed_us <= UINT64_MAX / UINT64_C(1024)) {
        rate->throughput_tenths_kib_s = mtfs_benchmark_mul_div(
            bytes, UINT64_C(10000000), elapsed_us * UINT64_C(1024));
    }
    rate->iops_tenths = mtfs_benchmark_mul_div(
        requests, UINT64_C(10000000), elapsed_us);
}

void mtfs_benchmark_latency_reset(mtfs_benchmark_latency_t *latency)
{
    if (latency != NULL) {
        latency->minimum_us = UINT64_MAX;
        latency->total_us = 0U;
        latency->maximum_us = 0U;
        latency->samples = 0U;
    }
}

void mtfs_benchmark_latency_add(
    mtfs_benchmark_latency_t *latency, uint64_t elapsed_us)
{
    if (latency == NULL) {
        return;
    }
    if (elapsed_us < latency->minimum_us) {
        latency->minimum_us = elapsed_us;
    }
    if (elapsed_us > latency->maximum_us) {
        latency->maximum_us = elapsed_us;
    }
    if (UINT64_MAX - latency->total_us < elapsed_us) {
        latency->total_us = UINT64_MAX;
    } else {
        latency->total_us += elapsed_us;
    }
    ++latency->samples;
}

static uint8_t mtfs_benchmark_pattern_byte(uint64_t offset)
{
    uint32_t mixed = (uint32_t)offset ^ (uint32_t)(offset >> 32U);
    mixed ^= UINT32_C(0xA5C31F27);
    mixed *= UINT32_C(2654435761);
    mixed ^= mixed >> 13U;
    return (uint8_t)(mixed ^ (mixed >> 8U) ^ (mixed >> 16U));
}

void mtfs_benchmark_pattern_fill(
    uint8_t *buffer, size_t length, uint64_t offset)
{
    size_t index;
    if (buffer == NULL) {
        return;
    }
    for (index = 0U; index < length; ++index) {
        buffer[index] = mtfs_benchmark_pattern_byte(offset + index);
    }
}

int mtfs_benchmark_pattern_verify(const uint8_t *buffer, size_t length,
    uint64_t offset, uint64_t *mismatch_offset)
{
    size_t index;
    if (buffer == NULL) {
        return 0;
    }
    for (index = 0U; index < length; ++index) {
        if (buffer[index] != mtfs_benchmark_pattern_byte(offset + index)) {
            if (mismatch_offset != NULL) {
                *mismatch_offset = offset + index;
            }
            return 0;
        }
    }
    return 1;
}

static uint32_t mtfs_benchmark_checksum(
    uint32_t checksum, const uint8_t *buffer, size_t length)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        checksum ^= buffer[index];
        checksum *= UINT32_C(16777619);
    }
    return checksum;
}

static int mtfs_benchmark_config_valid(const mtfs_benchmark_config_t *config)
{
    return (config != NULL) && (config->device != NULL) &&
        (config->volume_path != NULL) && (config->board_name != NULL) &&
        (config->build_name != NULL) && (config->clock_us != NULL) &&
        (config->log != NULL) && (config->buffer != NULL) &&
        (config->buffer_size >= MTFS_BENCHMARK_BUFFER_BYTES);
}

static const char *mtfs_benchmark_fs_name(BYTE fs_type)
{
    if (fs_type == FS_FAT12) {
        return "FAT12";
    }
    if (fs_type == FS_FAT16) {
        return "FAT16";
    }
    if (fs_type == FS_FAT32) {
        return "FAT32";
    }
    return "unknown";
}

static void mtfs_benchmark_log_error(const mtfs_benchmark_config_t *config,
    const char *stage, int32_t code)
{
    mtfs_benchmark_line_t line;
    mtfs_benchmark_line_begin(&line, "[BENCH] error stage=");
    mtfs_benchmark_line_text(&line, stage);
    mtfs_benchmark_line_text(&line, " code=");
    mtfs_benchmark_line_s32(&line, code);
    mtfs_benchmark_emit(config, &line);
}

static void mtfs_benchmark_log_config(const mtfs_benchmark_config_t *config,
    uint32_t request_bytes, uint64_t total_bytes, const char *sync_policy)
{
    mtfs_benchmark_line_t line;
    mtfs_benchmark_line_begin(&line, "[BENCH] config request_bytes=");
    mtfs_benchmark_line_u64(&line, request_bytes);
    mtfs_benchmark_line_text(&line, " total_bytes=");
    mtfs_benchmark_line_u64(&line, total_bytes);
    mtfs_benchmark_line_text(&line, " sync=");
    mtfs_benchmark_line_text(&line, sync_policy);
    mtfs_benchmark_emit(config, &line);
}

static void mtfs_benchmark_log_result(const mtfs_benchmark_config_t *config,
    uint64_t bytes, uint64_t requests, uint64_t elapsed_us,
    const mtfs_benchmark_latency_t *latency, uint32_t checksum)
{
    mtfs_benchmark_rate_t rate;
    mtfs_benchmark_line_t line;
    uint64_t average = latency->samples == 0U
        ? 0U : latency->total_us / latency->samples;
    uint64_t minimum = latency->samples == 0U ? 0U : latency->minimum_us;

    mtfs_benchmark_rate_calculate(bytes, requests, elapsed_us, &rate);
    mtfs_benchmark_line_begin(&line, "[BENCH] result bytes=");
    mtfs_benchmark_line_u64(&line, bytes);
    mtfs_benchmark_line_text(&line, " requests=");
    mtfs_benchmark_line_u64(&line, requests);
    mtfs_benchmark_line_text(&line, " elapsed_us=");
    mtfs_benchmark_line_u64(&line, elapsed_us);
    mtfs_benchmark_line_text(&line, " throughput_kib_s=");
    mtfs_benchmark_line_fixed1(&line, rate.throughput_tenths_kib_s);
    mtfs_benchmark_line_text(&line, " iops=");
    mtfs_benchmark_line_fixed1(&line, rate.iops_tenths);
    mtfs_benchmark_emit(config, &line);

    mtfs_benchmark_line_begin(&line, "[BENCH] latency_us min=");
    mtfs_benchmark_line_u64(&line, minimum);
    mtfs_benchmark_line_text(&line, " avg=");
    mtfs_benchmark_line_u64(&line, average);
    mtfs_benchmark_line_text(&line, " max=");
    mtfs_benchmark_line_u64(&line, latency->maximum_us);
    mtfs_benchmark_line_text(&line, " checksum=");
    mtfs_benchmark_line_u64(&line, checksum);
    mtfs_benchmark_emit(config, &line);
}

static void mtfs_benchmark_log_end(
    const mtfs_benchmark_config_t *config, int passed)
{
    mtfs_benchmark_line_t line;
    mtfs_benchmark_line_begin(&line, "[BENCH] END status=");
    mtfs_benchmark_line_text(&line, passed ? "PASS" : "FAIL");
    mtfs_benchmark_emit(config, &line);
}

int mtfs_benchmark_print_info(const mtfs_benchmark_config_t *config,
    mtfs_benchmark_profile_t profile)
{
    mtfs_block_geometry_t geometry;
    mtfs_benchmark_line_t line;
    uint64_t raw_bytes;
    uint64_t file_bytes;
    uint64_t sync_bytes;
    mtfs_error_t error;
    mtfs_benchmark_latency_t clock_latency;
    unsigned int sample;

    if (!mtfs_benchmark_config_valid(config) ||
        !mtfs_benchmark_profile_sizes(profile, &raw_bytes, &file_bytes,
            &sync_bytes)) {
        return 1;
    }
    error = mtfs_block_get_geometry(config->device, &geometry);
    if (error != MTFS_OK) {
        mtfs_benchmark_log_error(config, "geometry", error);
        return 1;
    }
    mtfs_benchmark_line_begin(&line, "[BENCH] info board=");
    mtfs_benchmark_line_text(&line, config->board_name);
    mtfs_benchmark_line_text(&line, " profile=");
    mtfs_benchmark_line_text(&line, mtfs_benchmark_profile_name(profile));
    mtfs_benchmark_line_text(&line, " build=");
    mtfs_benchmark_line_text(&line, config->build_name);
    mtfs_benchmark_emit(config, &line);

    mtfs_benchmark_line_begin(&line, "[BENCH] media sector_count=");
    mtfs_benchmark_line_u64(&line, geometry.sector_count);
    mtfs_benchmark_line_text(&line, " sector_size=");
    mtfs_benchmark_line_u64(&line, geometry.sector_size);
    mtfs_benchmark_line_text(&line, " erase_block_sectors=");
    mtfs_benchmark_line_u64(&line, geometry.erase_block_size);
    mtfs_benchmark_emit(config, &line);

    mtfs_benchmark_line_begin(&line, "[BENCH] profile raw_total_bytes=");
    mtfs_benchmark_line_u64(&line, raw_bytes);
    mtfs_benchmark_line_text(&line, " file_total_bytes=");
    mtfs_benchmark_line_u64(&line, file_bytes);
    mtfs_benchmark_line_text(&line, " sync_total_bytes=");
    mtfs_benchmark_line_u64(&line, sync_bytes);
    mtfs_benchmark_line_text(&line, " warmup=no runs=1");
    mtfs_benchmark_emit(config, &line);
    if (config->target_info != NULL) {
        config->target_info(config->target_info_context,
            config->log, config->log_context);
    }
    mtfs_benchmark_latency_reset(&clock_latency);
    for (sample = 0U; sample < 32U; ++sample) {
        uint64_t clock_begin = config->clock_us(config->clock_context);
        uint64_t clock_end = config->clock_us(config->clock_context);
        mtfs_benchmark_latency_add(&clock_latency, clock_end - clock_begin);
    }
    mtfs_benchmark_line_begin(&line, "[BENCH] clock_overhead_us min=");
    mtfs_benchmark_line_u64(&line, clock_latency.minimum_us);
    mtfs_benchmark_line_text(&line, " avg=");
    mtfs_benchmark_line_u64(&line,
        clock_latency.total_us / clock_latency.samples);
    mtfs_benchmark_line_text(&line, " max=");
    mtfs_benchmark_line_u64(&line, clock_latency.maximum_us);
    mtfs_benchmark_line_text(&line, " samples=");
    mtfs_benchmark_line_u64(&line, clock_latency.samples);
    mtfs_benchmark_emit(config, &line);
    return 0;
}

static int mtfs_benchmark_raw_read(const mtfs_benchmark_config_t *config,
    mtfs_benchmark_profile_t profile, uint64_t total_bytes,
    uint32_t request_bytes)
{
    mtfs_block_geometry_t geometry;
    mtfs_benchmark_latency_t latency;
    mtfs_benchmark_line_t line;
    mtfs_lba_t lba = 0U;
    uint64_t completed = 0U;
    uint64_t requests = 0U;
    uint64_t begin;
    uint64_t end;
    uint32_t checksum = UINT32_C(2166136261);
    uint32_t sectors;
    mtfs_error_t error;

    error = mtfs_block_get_geometry(config->device, &geometry);
    if (error != MTFS_OK) {
        mtfs_benchmark_log_error(config, "raw_geometry", error);
        return 1;
    }
    if ((geometry.sector_size == 0U) ||
        ((request_bytes % geometry.sector_size) != 0U)) {
        mtfs_benchmark_log_error(config, "raw_request_size",
            MTFS_ERROR_INVALID_ARGUMENT);
        return 1;
    }
    sectors = request_bytes / geometry.sector_size;
    if ((sectors == 0U) || ((mtfs_lba_t)sectors > geometry.sector_count)) {
        mtfs_benchmark_log_error(config, "raw_geometry_range",
            MTFS_ERROR_OUT_OF_RANGE);
        return 1;
    }

    mtfs_benchmark_line_begin(&line,
        "[BENCH] BEGIN name=block_seq_read profile=");
    mtfs_benchmark_line_text(&line, mtfs_benchmark_profile_name(profile));
    mtfs_benchmark_emit(config, &line);
    mtfs_benchmark_log_config(config, request_bytes, total_bytes, "none");
    mtfs_benchmark_latency_reset(&latency);
    begin = config->clock_us(config->clock_context);
    while (completed < total_bytes) {
        uint64_t request_begin;
        uint64_t request_end;
        if ((mtfs_lba_t)sectors > geometry.sector_count - lba) {
            lba = 0U;
        }
        request_begin = config->clock_us(config->clock_context);
        error = mtfs_block_read(config->device, config->buffer, lba, sectors);
        request_end = config->clock_us(config->clock_context);
        if (error != MTFS_OK) {
            mtfs_benchmark_log_error(config, "raw_read", error);
            mtfs_benchmark_log_end(config, 0);
            return 1;
        }
        mtfs_benchmark_latency_add(&latency, request_end - request_begin);
        checksum = mtfs_benchmark_checksum(checksum,
            config->buffer, request_bytes);
        completed += request_bytes;
        ++requests;
        lba += sectors;
    }
    end = config->clock_us(config->clock_context);
    mtfs_benchmark_log_result(config, completed, requests, end - begin,
        &latency, checksum);
    mtfs_benchmark_log_end(config, 1);
    return 0;
}

static int mtfs_benchmark_make_path(char *path, size_t path_size,
    const char *volume_path)
{
    size_t volume_length = strlen(volume_path);
    size_t need_separator = (volume_length != 0U) &&
        (volume_path[volume_length - 1U] != '/') ? 1U : 0U;
    size_t name_length = sizeof(MTFS_BENCHMARK_FILE_NAME) - 1U;
    if (volume_length + need_separator + name_length + 1U > path_size) {
        return 0;
    }
    memcpy(path, volume_path, volume_length);
    if (need_separator != 0U) {
        path[volume_length++] = '/';
    }
    memcpy(path + volume_length, MTFS_BENCHMARK_FILE_NAME, name_length + 1U);
    return 1;
}

static int mtfs_benchmark_file_case(const mtfs_benchmark_config_t *config,
    mtfs_benchmark_profile_t profile, const char *path,
    uint64_t total_bytes, uint32_t request_bytes, int sync_every_request)
{
    FIL file;
    FRESULT result;
    mtfs_benchmark_latency_t latency;
    mtfs_benchmark_line_t line;
    uint64_t offset = 0U;
    uint64_t write_begin;
    uint64_t write_end;
    uint64_t sync_begin;
    uint64_t sync_end;
    uint64_t close_end;
    uint64_t requests = 0U;
    uint32_t checksum = UINT32_C(2166136261);
    int file_open = 0;
    int file_created = 0;
    int failure = 0;

    mtfs_benchmark_line_begin(&line,
        "[BENCH] BEGIN name=fatfs_seq_write profile=");
    mtfs_benchmark_line_text(&line, mtfs_benchmark_profile_name(profile));
    mtfs_benchmark_emit(config, &line);
    mtfs_benchmark_log_config(config, request_bytes, total_bytes,
        sync_every_request ? "request" : "end");

    result = f_open(&file, path, FA_WRITE | FA_CREATE_NEW);
    if (result != FR_OK) {
        mtfs_benchmark_log_error(config, "file_create", result);
        mtfs_benchmark_log_end(config, 0);
        return 1;
    }
    file_open = 1;
    file_created = 1;
    mtfs_benchmark_latency_reset(&latency);
    write_begin = config->clock_us(config->clock_context);
    while (offset < total_bytes) {
        UINT written = 0U;
        uint64_t request_begin;
        uint64_t request_end;
        size_t length = (size_t)mtfs_benchmark_min_u64(
            request_bytes, total_bytes - offset);
        mtfs_benchmark_pattern_fill(config->buffer, length, offset);
        request_begin = config->clock_us(config->clock_context);
        result = f_write(&file, config->buffer, (UINT)length, &written);
        request_end = config->clock_us(config->clock_context);
        mtfs_benchmark_latency_add(&latency, request_end - request_begin);
        if ((result != FR_OK) || (written != length)) {
            mtfs_benchmark_log_error(config, "file_write",
                result != FR_OK ? result : FR_DISK_ERR);
            failure = 1;
            goto cleanup;
        }
        ++requests;
        offset += length;
        if (sync_every_request) {
            result = f_sync(&file);
            if (result != FR_OK) {
                mtfs_benchmark_log_error(config, "file_sync_request", result);
                failure = 1;
                goto cleanup;
            }
        }
    }
    write_end = config->clock_us(config->clock_context);
    sync_begin = config->clock_us(config->clock_context);
    result = f_sync(&file);
    sync_end = config->clock_us(config->clock_context);
    if (result != FR_OK) {
        mtfs_benchmark_log_error(config, "file_sync_final", result);
        failure = 1;
        goto cleanup;
    }
    result = f_close(&file);
    close_end = config->clock_us(config->clock_context);
    file_open = 0;
    if (result != FR_OK) {
        mtfs_benchmark_log_error(config, "file_close_write", result);
        failure = 1;
        goto cleanup;
    }
    mtfs_benchmark_log_result(config, offset, requests,
        write_end - write_begin, &latency, 0U);
    mtfs_benchmark_line_begin(&line, "[BENCH] timing data_write_us=");
    mtfs_benchmark_line_u64(&line, write_end - write_begin);
    mtfs_benchmark_line_text(&line, " final_sync_us=");
    mtfs_benchmark_line_u64(&line, sync_end - sync_begin);
    mtfs_benchmark_line_text(&line, " total_close_us=");
    mtfs_benchmark_line_u64(&line, close_end - write_begin);
    mtfs_benchmark_line_text(&line, " sync_calls=");
    mtfs_benchmark_line_u64(&line,
        sync_every_request ? requests + 1U : 1U);
    mtfs_benchmark_emit(config, &line);
    mtfs_benchmark_log_end(config, 1);

    mtfs_benchmark_line_begin(&line,
        "[BENCH] BEGIN name=fatfs_seq_read profile=");
    mtfs_benchmark_line_text(&line, mtfs_benchmark_profile_name(profile));
    mtfs_benchmark_emit(config, &line);
    mtfs_benchmark_log_config(config, request_bytes, total_bytes, "none");
    result = f_open(&file, path, FA_READ | FA_OPEN_EXISTING);
    if (result != FR_OK) {
        mtfs_benchmark_log_error(config, "file_open_read", result);
        failure = 1;
        goto cleanup;
    }
    file_open = 1;
    offset = 0U;
    requests = 0U;
    mtfs_benchmark_latency_reset(&latency);
    write_begin = config->clock_us(config->clock_context);
    while (offset < total_bytes) {
        UINT read_count = 0U;
        uint64_t request_begin;
        uint64_t request_end;
        uint64_t mismatch = 0U;
        size_t length = (size_t)mtfs_benchmark_min_u64(
            request_bytes, total_bytes - offset);
        request_begin = config->clock_us(config->clock_context);
        result = f_read(&file, config->buffer, (UINT)length, &read_count);
        request_end = config->clock_us(config->clock_context);
        mtfs_benchmark_latency_add(&latency, request_end - request_begin);
        if ((result != FR_OK) || (read_count != length)) {
            mtfs_benchmark_log_error(config, "file_read",
                result != FR_OK ? result : FR_DISK_ERR);
            failure = 1;
            goto cleanup;
        }
        checksum = mtfs_benchmark_checksum(checksum, config->buffer, length);
        if (!mtfs_benchmark_pattern_verify(config->buffer, length,
                offset, &mismatch)) {
            mtfs_benchmark_log_error(config, "pattern_mismatch",
                (int32_t)(mismatch > INT32_MAX ? INT32_MAX : mismatch));
            failure = 1;
            goto cleanup;
        }
        ++requests;
        offset += length;
    }
    write_end = config->clock_us(config->clock_context);
    result = f_close(&file);
    file_open = 0;
    if (result != FR_OK) {
        mtfs_benchmark_log_error(config, "file_close_read", result);
        failure = 1;
        goto cleanup;
    }
    mtfs_benchmark_log_result(config, offset, requests,
        write_end - write_begin, &latency, checksum);
    mtfs_benchmark_log_end(config, 1);

cleanup:
    if (file_open) {
        FRESULT close_result = f_close(&file);
        if (close_result != FR_OK) {
            mtfs_benchmark_log_error(config, "cleanup_close", close_result);
            failure = 1;
        }
    }
    if (file_created) {
        FRESULT unlink_result = f_unlink(path);
        if (unlink_result != FR_OK) {
            mtfs_benchmark_log_error(config, "cleanup_unlink", unlink_result);
            failure = 1;
        } else {
            mtfs_benchmark_line_begin(&line,
                "[BENCH] cleanup file_removed=yes");
            mtfs_benchmark_emit(config, &line);
        }
    }
    if (failure) {
        mtfs_benchmark_log_end(config, 0);
    }
    return failure;
}

int mtfs_benchmark_run(const mtfs_benchmark_config_t *config,
    mtfs_benchmark_profile_t profile)
{
    FATFS filesystem;
    FATFS *mounted_fs = NULL;
    FILINFO file_info;
    DWORD free_clusters = 0U;
    FRESULT result;
    mtfs_benchmark_line_t line;
    uint64_t raw_total;
    uint64_t file_total;
    uint64_t sync_total;
    uint64_t free_bytes;
    char path[32];
    unsigned int index;
    int mounted = 0;
    int failure = 0;

    if (!mtfs_benchmark_config_valid(config) ||
        !mtfs_benchmark_profile_sizes(profile, &raw_total, &file_total,
            &sync_total) ||
        !mtfs_benchmark_make_path(path, sizeof(path), config->volume_path)) {
        return 1;
    }
    mtfs_benchmark_line_begin(&line, "[BENCH] SUITE BEGIN profile=");
    mtfs_benchmark_line_text(&line, mtfs_benchmark_profile_name(profile));
    mtfs_benchmark_emit(config, &line);
    if (mtfs_benchmark_print_info(config, profile) != 0) {
        failure = 1;
        goto done;
    }

    for (index = 0U; index < MTFS_BENCHMARK_REQUEST_COUNT; ++index) {
        if (mtfs_benchmark_raw_read(config, profile, raw_total,
                mtfs_benchmark_request_bytes[index]) != 0) {
            failure = 1;
            goto done;
        }
    }

    result = f_mount(&filesystem, config->volume_path, 1U);
    if (result != FR_OK) {
        mtfs_benchmark_log_error(config, "mount", result);
        failure = 1;
        goto done;
    }
    mounted = 1;
    result = f_stat(path, &file_info);
    if (result == FR_OK) {
        mtfs_benchmark_log_error(config, "temp_file_exists", FR_EXIST);
        failure = 1;
        goto done;
    }
    if (result != FR_NO_FILE) {
        mtfs_benchmark_log_error(config, "temp_file_check", result);
        failure = 1;
        goto done;
    }
    result = f_getfree(config->volume_path, &free_clusters, &mounted_fs);
    if ((result != FR_OK) || (mounted_fs == NULL)) {
        mtfs_benchmark_log_error(config, "free_space", result);
        failure = 1;
        goto done;
    }
    free_bytes = (uint64_t)free_clusters * mounted_fs->csize * 512U;
    mtfs_benchmark_line_begin(&line, "[BENCH] filesystem type=");
    mtfs_benchmark_line_text(&line,
        mtfs_benchmark_fs_name(mounted_fs->fs_type));
    mtfs_benchmark_line_text(&line, " cluster_bytes=");
    mtfs_benchmark_line_u64(&line, (uint64_t)mounted_fs->csize * 512U);
    mtfs_benchmark_line_text(&line, " free_bytes=");
    mtfs_benchmark_line_u64(&line, free_bytes);
    mtfs_benchmark_emit(config, &line);
    if (!mtfs_benchmark_has_sufficient_space(free_bytes, file_total)) {
        mtfs_benchmark_log_error(config, "insufficient_space", FR_DENIED);
        failure = 1;
        goto done;
    }

    for (index = 0U; index < MTFS_BENCHMARK_REQUEST_COUNT; ++index) {
        if (mtfs_benchmark_file_case(config, profile, path, file_total,
                mtfs_benchmark_request_bytes[index], 0) != 0) {
            failure = 1;
            goto done;
        }
    }
    for (index = 0U; index < MTFS_BENCHMARK_REQUEST_COUNT; ++index) {
        if (mtfs_benchmark_file_case(config, profile, path, sync_total,
                mtfs_benchmark_request_bytes[index], 1) != 0) {
            failure = 1;
            goto done;
        }
    }

done:
    if (mounted) {
        result = f_mount(NULL, config->volume_path, 0U);
        if (result != FR_OK) {
            mtfs_benchmark_log_error(config, "unmount", result);
            failure = 1;
        }
    }
    mtfs_benchmark_line_begin(&line, "[BENCH] SUITE END status=");
    mtfs_benchmark_line_text(&line, failure ? "FAIL" : "PASS");
    mtfs_benchmark_emit(config, &line);
    return failure;
}
