#ifndef MTFS_STM32N6570_DK_PLATFORM_H
#define MTFS_STM32N6570_DK_PLATFORM_H

#include <stdint.h>

#include "mtfs_stm32_sdmmc.h"
#include "mtfs_media.h"
#include "mtfs_media_service.h"

#ifndef MTFS_STM32_SD_USE_IDMA
#define MTFS_STM32_SD_USE_IDMA (1U)
#endif

#if (MTFS_STM32_SD_USE_IDMA != 0) && (MTFS_STM32_SD_USE_IDMA != 1)
#error MTFS_STM32_SD_USE_IDMA must be 0 or 1
#endif

typedef struct mtfs_stm32n6570_dk_rif_diagnostics
{
    uint32_t master_attribute;
    uint32_t slave_secure;
    uint32_t slave_privileged;
    uint8_t ready;
} mtfs_stm32n6570_dk_rif_diagnostics_t;

typedef struct mtfs_stm32n6570_dk_card_detect_diagnostics
{
    uint32_t irq_entries;
    uint32_t rising_edges;
    uint32_t falling_edges;
    uint32_t service_notify_errors;
    uint32_t port_notify_errors;
    uint8_t raw_level;
    uint8_t active_low;
    uint8_t irq_registered;
} mtfs_stm32n6570_dk_card_detect_diagnostics_t;

/* Call after Cube peripheral setup and before starting microT-Kernel. */
/* Cube peripheral設定後、microT-Kernel起動前に呼び出す。 */
HAL_StatusTypeDef mtfs_stm32n6570_dk_pre_kernel_init(void);

uint64_t mtfs_stm32n6570_dk_benchmark_clock_us(void *context);
uint32_t mtfs_stm32n6570_dk_cycle_count(void);
uint32_t mtfs_stm32n6570_dk_cycle_clock_hz(void);
#if MTFS_ENABLE_STORAGE_SENTINEL
mtfs_error_t mtfs_stm32n6570_dk_sentinel_clock_us(
    void *context, uint64_t *now_us);
#endif
uint32_t mtfs_stm32n6570_dk_sdmmc_clock_hz(void);

void mtfs_stm32n6570_dk_sdmmc_config(
    mtfs_stm32_sdmmc_config_t *config);
void mtfs_stm32n6570_dk_get_rif_diagnostics(
    mtfs_stm32n6570_dk_rif_diagnostics_t *diagnostics);

mtfs_error_t mtfs_stm32n6570_dk_card_detect_start(
    mtfs_media_context_t *media,
    mtfs_media_service_context_t *service,
    mtfs_stm32_sdmmc_context_t *sdmmc,
    mtfs_media_event_fn event_callback,
    void *event_context);
mtfs_error_t mtfs_stm32n6570_dk_card_detect_stop(void);
void mtfs_stm32n6570_dk_get_card_detect_diagnostics(
    mtfs_stm32n6570_dk_card_detect_diagnostics_t *diagnostics);

#endif /* MTFS_STM32N6570_DK_PLATFORM_H */
