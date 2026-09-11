#ifndef MTFS_STM32_TARGET_CONCURRENT_H
#define MTFS_STM32_TARGET_CONCURRENT_H

#include "mtfs_test_concurrent_microtkernel.h"

typedef mtfs_tk_stack_watermark_t mtfs_target_stack_watermark_t;

int mtfs_target_run_concurrent(mtfs_test_t *test, const char *volume_path,
    unsigned int outer_iteration);
unsigned int mtfs_target_concurrent_stack_count(void);
int mtfs_target_concurrent_stack_watermark(unsigned int index,
    mtfs_target_stack_watermark_t *watermark);

#endif /* MTFS_STM32_TARGET_CONCURRENT_H */
