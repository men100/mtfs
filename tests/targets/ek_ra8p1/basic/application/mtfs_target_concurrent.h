#ifndef MTFS_TARGET_CONCURRENT_H
#define MTFS_TARGET_CONCURRENT_H

#include <stddef.h>

#include "mtfs_test.h"

typedef struct mtfs_target_stack_watermark
{
    size_t total_bytes;
    size_t used_bytes;
    size_t free_bytes;
    int guard_ok;
    int measured;
} mtfs_target_stack_watermark_t;

int mtfs_target_run_concurrent(mtfs_test_t *test, const char *volume_path,
    unsigned int outer_iteration);
unsigned int mtfs_target_concurrent_stack_count(void);
int mtfs_target_concurrent_stack_watermark(unsigned int index,
    mtfs_target_stack_watermark_t *watermark);

#endif /* MTFS_TARGET_CONCURRENT_H */
