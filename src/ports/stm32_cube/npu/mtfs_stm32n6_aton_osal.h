#ifndef MTFS_STM32N6_ATON_OSAL_H
#define MTFS_STM32N6_ATON_OSAL_H

#include <stdint.h>

#include "mtfs_stm32n6_async_wait.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mtfs_stm32n6_aton_osal_diagnostics
{
    uint32_t irq_notifications;
    uint32_t consumed_notifications;
    uint32_t spurious_notifications;
    uint32_t late_notifications;
    uint32_t kernel_signal_errors;
    int32_t last_kernel_error;
} mtfs_stm32n6_aton_osal_diagnostics_t;

void mtfs_stm32n6_aton_osal_init(void);
void mtfs_stm32n6_aton_osal_deinit(void);
void mtfs_stm32n6_aton_osal_wfe(void);
void mtfs_stm32n6_aton_osal_signal_event(void);

int mtfs_stm32n6_aton_osal_ready(void);
mtfs_error_t mtfs_stm32n6_aton_osal_inference_begin(void);
void mtfs_stm32n6_aton_osal_inference_end(void);
mtfs_stm32n6_event_wait_result_t mtfs_stm32n6_aton_osal_wait_event(
    uint32_t remaining_timeout_ms, int *immediate);
mtfs_error_t mtfs_stm32n6_aton_osal_diagnostics_reset(void);
void mtfs_stm32n6_aton_osal_diagnostics_get(
    mtfs_stm32n6_aton_osal_diagnostics_t *diagnostics);

#ifdef __cplusplus
}
#endif

#endif
