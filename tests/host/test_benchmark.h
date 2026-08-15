#ifndef TEST_BENCHMARK_H
#define TEST_BENCHMARK_H

#include "mtfs_block_device.h"
#include "mtfs_test.h"

int test_benchmark(mtfs_test_t *test, mtfs_block_device_t *device,
    const char *volume_path);

#endif /* TEST_BENCHMARK_H */
