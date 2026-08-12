#include "mtfs_stm32_hal_timebase.h"

#include <tk/tkernel.h>

#if CNF_TIMER_PERIOD == 1
#define MTFS_STM32_HAL_TICK_FREQ HAL_TICK_FREQ_1KHZ
#elif CNF_TIMER_PERIOD == 10
#define MTFS_STM32_HAL_TICK_FREQ HAL_TICK_FREQ_100HZ
#elif CNF_TIMER_PERIOD == 100
#define MTFS_STM32_HAL_TICK_FREQ HAL_TICK_FREQ_10HZ
#else
#error "STM32 HAL timebase requires CNF_TIMER_PERIOD to be 1, 10, or 100 ms"
#endif

static ID mtfs_stm32_hal_tick_cyclic_id;
static unsigned int mtfs_stm32_hal_tick_users;
static HAL_TickFreqTypeDef mtfs_stm32_previous_tick_frequency;

static void mtfs_stm32_hal_tick_handler(void *opaque)
{
    (void)opaque;
    HAL_IncTick();
}

HAL_StatusTypeDef mtfs_stm32_hal_timebase_acquire(void)
{
    T_CCYC cyclic = {0};
    ID cyclic_id;

    if (mtfs_stm32_hal_tick_users != 0U) {
        ++mtfs_stm32_hal_tick_users;
        return HAL_OK;
    }

    mtfs_stm32_previous_tick_frequency = uwTickFreq;
    /* HAL_SetTickFreq() would call HAL_InitTick() and take SysTick from T-Kernel. */
    uwTickFreq = MTFS_STM32_HAL_TICK_FREQ;
    cyclic.cycatr = TA_HLNG | TA_STA;
    cyclic.cychdr = mtfs_stm32_hal_tick_handler;
    cyclic.cyctim = CNF_TIMER_PERIOD;
    cyclic.cycphs = 0U;
    cyclic_id = tk_cre_cyc(&cyclic);
    if (cyclic_id <= 0) {
        uwTickFreq = mtfs_stm32_previous_tick_frequency;
        return HAL_ERROR;
    }

    mtfs_stm32_hal_tick_cyclic_id = cyclic_id;
    mtfs_stm32_hal_tick_users = 1U;
    return HAL_OK;
}

HAL_StatusTypeDef mtfs_stm32_hal_timebase_release(void)
{
    ER kernel_result;

    if (mtfs_stm32_hal_tick_users == 0U) {
        return HAL_ERROR;
    }
    --mtfs_stm32_hal_tick_users;
    if (mtfs_stm32_hal_tick_users != 0U) {
        return HAL_OK;
    }

    kernel_result = tk_stp_cyc(mtfs_stm32_hal_tick_cyclic_id);
    if (kernel_result >= E_OK) {
        kernel_result = tk_del_cyc(mtfs_stm32_hal_tick_cyclic_id);
    }
    mtfs_stm32_hal_tick_cyclic_id = 0;
    uwTickFreq = mtfs_stm32_previous_tick_frequency;
    return (kernel_result >= E_OK) ? HAL_OK : HAL_ERROR;
}
