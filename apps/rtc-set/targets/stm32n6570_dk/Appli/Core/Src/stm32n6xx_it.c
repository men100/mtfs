#include "main.h"
#include "stm32n6xx_it.h"

static void fault_loop(void)
{
    for (;;) {
    }
}

void NMI_Handler(void)
{
    fault_loop();
}

void HardFault_Handler(void)
{
    fault_loop();
}

void MemManage_Handler(void)
{
    fault_loop();
}

void BusFault_Handler(void)
{
    fault_loop();
}

void UsageFault_Handler(void)
{
    fault_loop();
}

void SecureFault_Handler(void)
{
    fault_loop();
}

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}

void SysTick_Handler(void)
{
    HAL_IncTick();
}
