/* microT-Kernel-owned HAL timebase for STM32Cube targets. */
#ifndef MTFS_STM32_HAL_TIMEBASE_H
#define MTFS_STM32_HAL_TIMEBASE_H

#ifndef MTFS_STM32_HAL_HEADER
#define MTFS_STM32_HAL_HEADER "stm32n6xx_hal.h"
#endif
#include MTFS_STM32_HAL_HEADER

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start/stop a reference-counted cyclic handler which advances uwTick.
 * These functions must be called from task context after microT-Kernel starts.
 */
HAL_StatusTypeDef mtfs_stm32_hal_timebase_acquire(void);
HAL_StatusTypeDef mtfs_stm32_hal_timebase_release(void);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_STM32_HAL_TIMEBASE_H */
