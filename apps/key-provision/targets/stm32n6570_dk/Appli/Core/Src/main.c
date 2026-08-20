#include "main.h"

void knl_start_mtkernel(void);

int main(void)
{
    SCB_EnableICache();
    SCB_EnableDCache();
    HAL_Init();

    knl_start_mtkernel();

    for (;;) {
    }
}

void Error_Handler(void)
{
    __disable_irq();
    for (;;) {
    }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
}
#endif
