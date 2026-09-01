#ifndef MTFS_SENTINEL_LAB_INJECTOR_H
#define MTFS_SENTINEL_LAB_INJECTOR_H

#include <stdint.h>

#include "mtfs_block_device.h"

#define MTFS_SENTINEL_LAB_INJECT_READ  (UINT32_C(1) << 0)
#define MTFS_SENTINEL_LAB_INJECT_WRITE (UINT32_C(1) << 1)
#define MTFS_SENTINEL_LAB_INJECT_SYNC  (UINT32_C(1) << 2)
#define MTFS_SENTINEL_LAB_INJECT_ALL \
    (MTFS_SENTINEL_LAB_INJECT_READ | MTFS_SENTINEL_LAB_INJECT_WRITE | \
     MTFS_SENTINEL_LAB_INJECT_SYNC)

typedef void (*mtfs_sentinel_lab_delay_fn)(void *context, uint32_t delay_us);

typedef enum mtfs_sentinel_lab_injection_kind
{
    MTFS_SENTINEL_LAB_INJECTION_NONE = 0,
    MTFS_SENTINEL_LAB_INJECTION_DELAY = 1,
    MTFS_SENTINEL_LAB_INJECTION_ERROR = 2
} mtfs_sentinel_lab_injection_kind_t;

typedef struct mtfs_sentinel_lab_injector_config
{
    mtfs_block_device_t *downstream;
    mtfs_sentinel_lab_delay_fn delay;
    void *delay_context;
} mtfs_sentinel_lab_injector_config_t;

typedef struct mtfs_sentinel_lab_injection
{
    mtfs_sentinel_lab_injection_kind_t kind;
    uint32_t operation_mask;
    uint32_t rate_permille;
    uint32_t delay_us;
    mtfs_error_t error;
    uint32_t seed;
} mtfs_sentinel_lab_injection_t;

typedef struct mtfs_sentinel_lab_injector
{
    mtfs_block_device_t device;
    mtfs_sentinel_lab_injector_config_t config;
    mtfs_sentinel_lab_injection_t injection;
    uint32_t random_state;
    uint32_t eligible_count;
    uint32_t injection_count;
    uint8_t initialized;
} mtfs_sentinel_lab_injector_t;

mtfs_error_t mtfs_sentinel_lab_injector_init(
    mtfs_sentinel_lab_injector_t *injector,
    const mtfs_sentinel_lab_injector_config_t *config);
mtfs_block_device_t *mtfs_sentinel_lab_injector_block_device(
    mtfs_sentinel_lab_injector_t *injector);
mtfs_error_t mtfs_sentinel_lab_injector_enable(
    mtfs_sentinel_lab_injector_t *injector,
    const mtfs_sentinel_lab_injection_t *injection);
void mtfs_sentinel_lab_injector_disable(
    mtfs_sentinel_lab_injector_t *injector);
void mtfs_sentinel_lab_injector_reset_statistics(
    mtfs_sentinel_lab_injector_t *injector);
uint32_t mtfs_sentinel_lab_injector_injection_count(
    const mtfs_sentinel_lab_injector_t *injector);
uint32_t mtfs_sentinel_lab_injector_eligible_count(
    const mtfs_sentinel_lab_injector_t *injector);

#endif
