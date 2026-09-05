#include "mtfs_ra8p1_ethosu_hooks.h"

#include <stddef.h>

#include "bsp_api.h"
#include "mtfs_ra8p1_platform.h"

#define MTFS_ETHOSU_SEMAPHORE_POOL_SIZE (2U)
#define MTFS_ETHOSU_INFERENCE_TIMEOUT_US (UINT64_C(2000000))

typedef struct mtfs_ethosu_semaphore {
    volatile uint32_t count;
    uint8_t in_use;
    uint8_t slot;
} mtfs_ethosu_semaphore_t;

static mtfs_ethosu_semaphore_t semaphore_pool[MTFS_ETHOSU_SEMAPHORE_POOL_SIZE];
static mtfs_ra8p1_ethosu_hook_diagnostics_t hook_diagnostics;

void mtfs_ra8p1_ethosu_hook_diagnostics_reset(void)
{
    hook_diagnostics.creates = 0U;
    hook_diagnostics.create_failures = 0U;
    hook_diagnostics.destroys = 0U;
    hook_diagnostics.takes = 0U;
    hook_diagnostics.gives = 0U;
    hook_diagnostics.wfe_calls = 0U;
    hook_diagnostics.timeouts = 0U;
    hook_diagnostics.peak_in_use = 0U;
    hook_diagnostics.wait_us = 0U;
}

void mtfs_ra8p1_ethosu_hook_diagnostics_get(
    mtfs_ra8p1_ethosu_hook_diagnostics_t *diagnostics)
{
    if (diagnostics != NULL) {
        *diagnostics = hook_diagnostics;
    }
}

void *ethosu_semaphore_create(void)
{
    uint32_t index;
    uint32_t active = 0U;

    for (index = 0U; index < MTFS_ETHOSU_SEMAPHORE_POOL_SIZE; ++index) {
        if (semaphore_pool[index].in_use == 0U) {
            semaphore_pool[index].count = 0U;
            semaphore_pool[index].in_use = 1U;
            semaphore_pool[index].slot = (uint8_t)index;
            ++hook_diagnostics.creates;
            for (uint32_t scan = 0U; scan < MTFS_ETHOSU_SEMAPHORE_POOL_SIZE;
                    ++scan) {
                active += semaphore_pool[scan].in_use != 0U ? 1U : 0U;
            }
            if (active > hook_diagnostics.peak_in_use) {
                hook_diagnostics.peak_in_use = active;
            }
            return &semaphore_pool[index];
        }
    }
    ++hook_diagnostics.create_failures;
    return NULL;
}

void ethosu_semaphore_destroy(void *semaphore)
{
    mtfs_ethosu_semaphore_t *item = (mtfs_ethosu_semaphore_t *)semaphore;
    if (item != NULL && item >= &semaphore_pool[0] &&
        item < &semaphore_pool[MTFS_ETHOSU_SEMAPHORE_POOL_SIZE] &&
        item->slot != 0U && item->in_use != 0U) {
        item->count = 0U;
        item->in_use = 0U;
        ++hook_diagnostics.destroys;
    }
}

int ethosu_semaphore_take(void *semaphore, uint64_t timeout)
{
    mtfs_ethosu_semaphore_t *item = (mtfs_ethosu_semaphore_t *)semaphore;
    uint64_t start;
    uint64_t now;
    uint64_t hard_timeout;

    if (item == NULL || item < &semaphore_pool[0] ||
        item >= &semaphore_pool[MTFS_ETHOSU_SEMAPHORE_POOL_SIZE] ||
        item->in_use == 0U) {
        return -1;
    }
    ++hook_diagnostics.takes;
    start = mtfs_ra8p1_benchmark_clock_us(NULL);
    hard_timeout = item->slot == 0U ? UINT64_MAX :
        MTFS_ETHOSU_INFERENCE_TIMEOUT_US;
    if (timeout != UINT64_MAX && timeout < hard_timeout) {
        hard_timeout = timeout;
    }
    while (item->count == 0U) {
        if (hard_timeout != UINT64_MAX) {
            now = mtfs_ra8p1_benchmark_clock_us(NULL);
            if ((now - start) >= hard_timeout) {
                hook_diagnostics.wait_us += now - start;
                ++hook_diagnostics.timeouts;
                return -1;
            }
        }
        ++hook_diagnostics.wfe_calls;
        __WFE();
    }
    __DMB();
    --item->count;
    now = mtfs_ra8p1_benchmark_clock_us(NULL);
    hook_diagnostics.wait_us += now - start;
    return 0;
}

int ethosu_semaphore_give(void *semaphore)
{
    mtfs_ethosu_semaphore_t *item = (mtfs_ethosu_semaphore_t *)semaphore;
    if (item == NULL || item < &semaphore_pool[0] ||
        item >= &semaphore_pool[MTFS_ETHOSU_SEMAPHORE_POOL_SIZE] ||
        item->in_use == 0U) {
        return -1;
    }
    if (item->count == 0U) {
        item->count = 1U;
    }
    __DMB();
    ++hook_diagnostics.gives;
    __SEV();
    return 0;
}
