#include "mtfs_target_concurrent.h"

#include "mtfs_test_concurrent_microtkernel.h"

int mtfs_target_run_concurrent(mtfs_test_t *test, const char *volume_path,
    unsigned int outer_iteration)
{
    return mtfs_test_concurrent_microtkernel(
        test, volume_path, outer_iteration);
}
