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
#ifndef MTFS_STM32N6_CD_DEBOUNCE_MS
/* STM32N6570-DK Cube FileX hot-plug examples also settle CD for 500 ms. */
#define MTFS_STM32N6_CD_DEBOUNCE_MS            (500U)
#endif
#define MTFS_STM32N6_CD_IRQ_PRIORITY           (6U)

extern SD_HandleTypeDef hsd2;

static mtfs_stm32n6570_dk_rif_diagnostics_t rif_diagnostics;
static mtfs_stm32n6570_dk_card_detect_diagnostics_t cd_diagnostics;
static mtfs_media_service_context_t *cd_service;
static mtfs_stm32_sdmmc_context_t *cd_sdmmc;

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

static int mtfs_stm32n6570_dk_card_detect_raw(void *opaque)
{
    (void)opaque;
    return HAL_GPIO_ReadPin(SD_DETECT_GPIO_Port, SD_DETECT_Pin) ==
        GPIO_PIN_SET;
}

static uint32_t mtfs_stm32n6570_dk_now_ms(void *opaque)
{
    (void)opaque;
    return HAL_GetTick();
}

static void mtfs_stm32n6570_dk_exti12_handler(UINT interrupt_number)
{
    int rising = __HAL_GPIO_EXTI_GET_RISING_IT(SD_DETECT_Pin) != 0U;
    int falling = __HAL_GPIO_EXTI_GET_FALLING_IT(SD_DETECT_Pin) != 0U;
    int raw_level;
    (void)interrupt_number;

    if (!rising && !falling) {
        return;
    }
    __HAL_GPIO_EXTI_CLEAR_IT(SD_DETECT_Pin);
    raw_level = mtfs_stm32n6570_dk_card_detect_raw(NULL);
    ++cd_diagnostics.irq_entries;
    cd_diagnostics.rising_edges += rising ? 1U : 0U;
    cd_diagnostics.falling_edges += falling ? 1U : 0U;
    cd_diagnostics.raw_level = (uint8_t)raw_level;

    if ((cd_service == NULL) ||
        (mtfs_media_service_notify_isr(cd_service, raw_level) != MTFS_OK)) {
        ++cd_diagnostics.service_notify_errors;
    }
    /* PN12 is active-low, so a high edge is an immediate removal hint. */
    if ((cd_sdmmc != NULL) && raw_level &&
        (mtfs_stm32_sdmmc_media_changed_isr(cd_sdmmc, 0) != MTFS_OK)) {
        ++cd_diagnostics.port_notify_errors;
    }
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

mtfs_error_t mtfs_stm32n6570_dk_card_detect_start(
    mtfs_media_context_t *media,
    mtfs_media_service_context_t *service,
    mtfs_stm32_sdmmc_context_t *sdmmc,
    mtfs_media_event_fn event_callback,
    void *event_context)
{
    mtfs_media_config_t media_config;
    mtfs_media_service_config_t service_config;
    T_DINT interrupt = {0};
    mtfs_error_t result;

    if ((media == NULL) || (service == NULL) || (sdmmc == NULL) ||
        (cd_service != NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    (void)memset(&cd_diagnostics, 0, sizeof(cd_diagnostics));
    cd_diagnostics.active_low = 1U;
    cd_diagnostics.raw_level =
        (uint8_t)mtfs_stm32n6570_dk_card_detect_raw(NULL);

    (void)memset(&media_config, 0, sizeof(media_config));
    media_config.read_signal = mtfs_stm32n6570_dk_card_detect_raw;
    media_config.event_callback = event_callback;
    media_config.event_context = event_context;
    media_config.debounce_ms = MTFS_STM32N6_CD_DEBOUNCE_MS;
    media_config.active_level = MTFS_MEDIA_ACTIVE_LOW;
    result = mtfs_media_init(media, &media_config);
    if (result != MTFS_OK) {
        if (media->initialized) {
            (void)mtfs_media_deinit(media);
        }
        return result;
    }

    (void)memset(&service_config, 0, sizeof(service_config));
    service_config.media = media;
    service_config.now_ms = mtfs_stm32n6570_dk_now_ms;
    service_config.task_priority = 10;
    result = mtfs_media_service_init(service, &service_config);
    if (result != MTFS_OK) {
        (void)mtfs_media_deinit(media);
        return result;
    }

    cd_service = service;
    cd_sdmmc = sdmmc;
    __HAL_GPIO_EXTI_CLEAR_IT(SD_DETECT_Pin);
    interrupt.intatr = TA_HLNG;
    interrupt.inthdr = (FP)mtfs_stm32n6570_dk_exti12_handler;
    service->last_kernel_error = tk_def_int((UINT)EXTI12_IRQn, &interrupt);
    if (service->last_kernel_error < E_OK) {
        cd_sdmmc = NULL;
        (void)mtfs_media_service_stop_notifications(service);
        if (mtfs_media_service_deinit(service) == MTFS_OK) {
            cd_service = NULL;
            (void)mtfs_media_deinit(media);
        }
        return MTFS_ERROR_NOT_READY;
    }
    cd_diagnostics.irq_registered = 1U;
    HAL_NVIC_SetPriority(EXTI12_IRQn, MTFS_STM32N6_CD_IRQ_PRIORITY, 0U);
    HAL_NVIC_EnableIRQ(EXTI12_IRQn);
    return MTFS_OK;
}

mtfs_error_t mtfs_stm32n6570_dk_card_detect_stop(void)
{
    mtfs_media_service_context_t *service = cd_service;
    mtfs_media_context_t *media;
    mtfs_error_t service_result;
    mtfs_error_t result = MTFS_OK;

    if ((service == NULL) || (service->config.media == NULL)) {
        return MTFS_ERROR_NOT_READY;
    }
    media = service->config.media;
    if (mtfs_media_service_stop_notifications(service) != MTFS_OK) {
        result = MTFS_ERROR_IO;
    }
    HAL_NVIC_DisableIRQ(EXTI12_IRQn);
    __HAL_GPIO_EXTI_CLEAR_IT(SD_DETECT_Pin);
    if (tk_def_int((UINT)EXTI12_IRQn, NULL) < E_OK) {
        result = MTFS_ERROR_IO;
    }
    cd_diagnostics.irq_registered = 0U;
    cd_sdmmc = NULL;
    service_result = mtfs_media_service_deinit(service);
    if (service_result != MTFS_OK) {
        /*
         * Retain cd_service as an ownership guard.  Reusing its static stack
         * or media context while the worker may still exist is unsafe.
         */
        return MTFS_ERROR_IO;
    }
    cd_service = NULL;
    if (mtfs_media_deinit(media) != MTFS_OK) {
        result = MTFS_ERROR_IO;
    }
    return result;
}

void mtfs_stm32n6570_dk_get_card_detect_diagnostics(
    mtfs_stm32n6570_dk_card_detect_diagnostics_t *diagnostics)
{
    if (diagnostics != NULL) {
        *diagnostics = cd_diagnostics;
        diagnostics->raw_level =
            (uint8_t)mtfs_stm32n6570_dk_card_detect_raw(NULL);
    }
}
