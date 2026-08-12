#ifndef MTFS_STM32_TARGET_CONCURRENT_H
#define MTFS_STM32_TARGET_CONCURRENT_H

#include "mtfs_test.h"

int mtfs_target_run_concurrent(mtfs_test_t *test, const char *volume_path,
    unsigned int outer_iteration);

#endif /* MTFS_STM32_TARGET_CONCURRENT_H */
