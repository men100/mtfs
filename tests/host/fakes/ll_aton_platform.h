#ifndef TEST_FAKE_LL_ATON_PLATFORM_H
#define TEST_FAKE_LL_ATON_PLATFORM_H

#include <stdint.h>

#define NPU0_IRQn (21)
void NVIC_SetPriority(int32_t interrupt, uint32_t priority);

#endif
