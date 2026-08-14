/* STM32Cube HAL calendar RTC provider for microT-FS. */
#ifndef MTFS_STM32_RTC_H
#define MTFS_STM32_RTC_H

#include "stm32n6xx_hal.h"

#include "../../../core/mtfs_time.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_STM32_RTC_MARKER_REGISTER_MAGIC    RTC_BKP_DR28
#define MTFS_STM32_RTC_MARKER_REGISTER_VERSION  RTC_BKP_DR29
#define MTFS_STM32_RTC_MARKER_REGISTER_INVERSE  RTC_BKP_DR30

typedef struct mtfs_stm32_rtc_context
{
    RTC_HandleTypeDef rtc;
    mtfs_time_provider_t provider;
    uint32_t reset_flags_at_init;
    int initialized;
} mtfs_stm32_rtc_context_t;

mtfs_error_t mtfs_stm32_rtc_init(
    mtfs_stm32_rtc_context_t *context,
    mtfs_error_t (*lock)(void *lock_context),
    void (*unlock)(void *lock_context),
    void *lock_context);

mtfs_time_provider_t *mtfs_stm32_rtc_provider(
    mtfs_stm32_rtc_context_t *context);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_STM32_RTC_H */
