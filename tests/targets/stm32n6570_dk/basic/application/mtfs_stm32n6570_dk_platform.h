#ifndef MTFS_STM32N6570_DK_PLATFORM_H
#define MTFS_STM32N6570_DK_PLATFORM_H

#include <stdint.h>

#include "mtfs_stm32_sdmmc.h"

typedef struct mtfs_stm32n6570_dk_rif_diagnostics
{
    uint32_t master_attribute;
    uint32_t slave_secure;
    uint32_t slave_privileged;
    uint8_t ready;
} mtfs_stm32n6570_dk_rif_diagnostics_t;

/* Call after Cube peripheral setup and before starting microT-Kernel. */
HAL_StatusTypeDef mtfs_stm32n6570_dk_pre_kernel_init(void);

void mtfs_stm32n6570_dk_sdmmc_config(
    mtfs_stm32_sdmmc_config_t *config);
void mtfs_stm32n6570_dk_get_rif_diagnostics(
    mtfs_stm32n6570_dk_rif_diagnostics_t *diagnostics);

#endif /* MTFS_STM32N6570_DK_PLATFORM_H */
