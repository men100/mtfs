#include "mtfs_stm32n6570_dk_platform.h"

#include <string.h>

#include "main.h"

#ifndef MTFS_STM32_SD_USE_IDMA
#define MTFS_STM32_SD_USE_IDMA (1U)
#endif

#define MTFS_STM32N6_SDMMC2_RIMC_MASTER_INDEX (3U)
#define MTFS_STM32N6_SDMMC2_SLAVE_BIT          (UINT32_C(1) << 22)
#define MTFS_STM32N6_SDMMC2_SLAVE_WORD         (1U)
#define MTFS_STM32N6_SDMMC2_MASTER_CID         (1U)

extern SD_HandleTypeDef hsd2;

static mtfs_stm32n6570_dk_rif_diagnostics_t rif_diagnostics;

static void mtfs_stm32n6570_dk_read_rif(void)
{
    const uint32_t master_mask = RIFSC_RIMC_ATTRx_MCID |
        RIFSC_RIMC_ATTRx_MSEC | RIFSC_RIMC_ATTRx_MPRIV;
    const uint32_t master_value =
        (MTFS_STM32N6_SDMMC2_MASTER_CID << RIFSC_RIMC_ATTRx_MCID_Pos) |
        RIFSC_RIMC_ATTRx_MSEC | RIFSC_RIMC_ATTRx_MPRIV;

    rif_diagnostics.master_attribute =
        RIFSC->RIMC_ATTRx[MTFS_STM32N6_SDMMC2_RIMC_MASTER_INDEX];
    rif_diagnostics.slave_secure =
        RIFSC->RISC_SECCFGRx[MTFS_STM32N6_SDMMC2_SLAVE_WORD];
    rif_diagnostics.slave_privileged =
        RIFSC->RISC_PRIVCFGRx[MTFS_STM32N6_SDMMC2_SLAVE_WORD];
    rif_diagnostics.ready =
        ((rif_diagnostics.master_attribute & master_mask) == master_value) &&
        ((rif_diagnostics.slave_secure &
            MTFS_STM32N6_SDMMC2_SLAVE_BIT) != 0U) &&
        ((rif_diagnostics.slave_privileged &
            MTFS_STM32N6_SDMMC2_SLAVE_BIT) != 0U);
}

HAL_StatusTypeDef mtfs_stm32n6570_dk_pre_kernel_init(void)
{
    const uint32_t master_mask = RIFSC_RIMC_ATTRx_MCID |
        RIFSC_RIMC_ATTRx_MSEC | RIFSC_RIMC_ATTRx_MPRIV;
    const uint32_t master_value =
        (MTFS_STM32N6_SDMMC2_MASTER_CID << RIFSC_RIMC_ATTRx_MCID_Pos) |
        RIFSC_RIMC_ATTRx_MSEC | RIFSC_RIMC_ATTRx_MPRIV;

    __HAL_RCC_RIFSC_CLK_ENABLE();
    MODIFY_REG(
        RIFSC->RIMC_ATTRx[MTFS_STM32N6_SDMMC2_RIMC_MASTER_INDEX],
        master_mask, master_value);
    SET_BIT(RIFSC->RISC_SECCFGRx[MTFS_STM32N6_SDMMC2_SLAVE_WORD],
        MTFS_STM32N6_SDMMC2_SLAVE_BIT);
    SET_BIT(RIFSC->RISC_PRIVCFGRx[MTFS_STM32N6_SDMMC2_SLAVE_WORD],
        MTFS_STM32N6_SDMMC2_SLAVE_BIT);
    __DSB();
    mtfs_stm32n6570_dk_read_rif();
    return rif_diagnostics.ready ? HAL_OK : HAL_ERROR;
}

static int mtfs_stm32n6570_dk_card_present(void *opaque)
{
    (void)opaque;
    return HAL_GPIO_ReadPin(SD_DETECT_GPIO_Port, SD_DETECT_Pin) ==
        GPIO_PIN_RESET;
}

static int mtfs_stm32n6570_dk_idma_ready(void *opaque)
{
    (void)opaque;
    mtfs_stm32n6570_dk_read_rif();
    return rif_diagnostics.ready;
}

void mtfs_stm32n6570_dk_sdmmc_config(
    mtfs_stm32_sdmmc_config_t *config)
{
    if (config == NULL) {
        return;
    }
    (void)memset(config, 0, sizeof(*config));
    config->hal_sd = &hsd2;
    config->irq_number = SDMMC2_IRQn;
    config->card_present = mtfs_stm32n6570_dk_card_present;
    /* STM32N6570-DK exposes no write-protect input for this socket. */
    config->write_protected = NULL;
    config->idma_platform_ready = mtfs_stm32n6570_dk_idma_ready;
    config->io_timeout_ms = 5000U;
    config->transfer_timeout_ms = 5000U;
    config->use_idma = MTFS_STM32_SD_USE_IDMA;
    if (!config->use_idma) {
        hsd2.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_ENABLE;
    }
    config->manage_hal_timebase = 1U;
}

void mtfs_stm32n6570_dk_get_rif_diagnostics(
    mtfs_stm32n6570_dk_rif_diagnostics_t *diagnostics)
{
    if (diagnostics != NULL) {
        mtfs_stm32n6570_dk_read_rif();
        *diagnostics = rif_diagnostics;
    }
}
