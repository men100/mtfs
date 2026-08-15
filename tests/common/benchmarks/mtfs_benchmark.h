#ifndef MTFS_BENCHMARK_H
#define MTFS_BENCHMARK_H

#include <stddef.h>
#include <stdint.h>

#include "ff.h"
#include "mtfs_block_device.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_BENCHMARK_BUFFER_BYTES (32U * 1024U)
#define MTFS_BENCHMARK_FILE_NAME    "MTFSBEN.TMP"

typedef enum mtfs_benchmark_profile
{
    MTFS_BENCHMARK_PROFILE_SMOKE = 0,
    MTFS_BENCHMARK_PROFILE_NORMAL
} mtfs_benchmark_profile_t;

typedef uint64_t (*mtfs_benchmark_clock_fn)(void *context);
typedef void (*mtfs_benchmark_log_fn)(void *context, const char *line);
typedef void (*mtfs_benchmark_target_info_fn)(
    void *context, mtfs_benchmark_log_fn log, void *log_context);

typedef struct mtfs_benchmark_rate
{
    uint64_t throughput_tenths_kib_s;
    uint64_t iops_tenths;
} mtfs_benchmark_rate_t;

typedef struct mtfs_benchmark_latency
{
    uint64_t minimum_us;
    uint64_t total_us;
    uint64_t maximum_us;
    uint64_t samples;
} mtfs_benchmark_latency_t;

typedef struct mtfs_benchmark_config
{
    mtfs_block_device_t *device;
    const char *volume_path;
    const char *board_name;
    const char *build_name;
    mtfs_benchmark_clock_fn clock_us;
    void *clock_context;
    mtfs_benchmark_log_fn log;
    void *log_context;
    mtfs_benchmark_target_info_fn target_info;
    void *target_info_context;
    uint8_t *buffer;
    size_t buffer_size;
} mtfs_benchmark_config_t;

const char *mtfs_benchmark_profile_name(mtfs_benchmark_profile_t profile);
int mtfs_benchmark_profile_sizes(mtfs_benchmark_profile_t profile,
    uint64_t *raw_total_bytes, uint64_t *file_total_bytes,
    uint64_t *sync_total_bytes);
int mtfs_benchmark_has_sufficient_space(
    uint64_t free_bytes, uint64_t required_bytes);
void mtfs_benchmark_rate_calculate(uint64_t bytes, uint64_t requests,
    uint64_t elapsed_us, mtfs_benchmark_rate_t *rate);
void mtfs_benchmark_latency_reset(mtfs_benchmark_latency_t *latency);
void mtfs_benchmark_latency_add(
    mtfs_benchmark_latency_t *latency, uint64_t elapsed_us);
void mtfs_benchmark_pattern_fill(
    uint8_t *buffer, size_t length, uint64_t offset);
int mtfs_benchmark_pattern_verify(const uint8_t *buffer, size_t length,
    uint64_t offset, uint64_t *mismatch_offset);

int mtfs_benchmark_print_info(const mtfs_benchmark_config_t *config,
    mtfs_benchmark_profile_t profile);
int mtfs_benchmark_run(const mtfs_benchmark_config_t *config,
    mtfs_benchmark_profile_t profile);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_BENCHMARK_H */
