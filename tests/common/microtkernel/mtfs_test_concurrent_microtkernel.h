#ifndef MTFS_TEST_CONCURRENT_MICROTKERNEL_H
#define MTFS_TEST_CONCURRENT_MICROTKERNEL_H

#include <stddef.h>

#include "mtfs_test.h"

typedef struct mtfs_tk_stack_watermark
{
    size_t total_bytes;
    size_t used_bytes;
    size_t free_bytes;
    int guard_ok;
    int measured;
} mtfs_tk_stack_watermark_t;

int mtfs_test_concurrent_microtkernel(mtfs_test_t *test,
    const char *volume_path, unsigned int outer_iteration);
unsigned int mtfs_test_concurrent_microtkernel_stack_count(void);
int mtfs_test_concurrent_microtkernel_stack_watermark(unsigned int index,
    mtfs_tk_stack_watermark_t *watermark);

#endif /* MTFS_TEST_CONCURRENT_MICROTKERNEL_H */
