#include "mtfs_stm32n6570_dk_platform.h"

#include <string.h>

#include "main.h"
#include "mtfs_stm32n6570_dk_board_config.h"

#ifndef MTFS_STM32_SD_USE_IDMA
#define MTFS_STM32_SD_USE_IDMA (1U)
#endif

#define MTFS_STM32N6_SDMMC2_RIMC_MASTER_INDEX (3U)
#define MTFS_STM32N6_SDMMC2_SLAVE_BIT          (UINT32_C(1) << 22)
#define MTFS_STM32N6_SDMMC2_SLAVE_WORD         (1U)
#define MTFS_STM32N6_SDMMC2_MASTER_CID         (1U)
#ifndef MTFS_STM32N6_CD_DEBOUNCE_MS
#define MTFS_STM32N6_CD_DEBOUNCE_MS \
    MTFS_STM32N6570_DK_SD_CARD_DETECT_DEBOUNCE_MS
#endif
#define MTFS_STM32N6_CD_IRQ_PRIORITY \
    MTFS_STM32N6570_DK_SD_CARD_DETECT_IRQ_PRIORITY

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
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
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

uint32_t mtfs_stm32n6570_dk_cycle_count(void)
{
    return DWT->CYCCNT;
}

uint32_t mtfs_stm32n6570_dk_cycle_clock_hz(void)
{
    return SystemCoreClock;
}

static int mtfs_stm32n6570_dk_card_present(void *opaque)
{
    GPIO_PinState state;
    (void)opaque;
    state = HAL_GPIO_ReadPin(MTFS_STM32N6570_DK_SD_CARD_DETECT_PORT,
        MTFS_STM32N6570_DK_SD_CARD_DETECT_PIN);
    return MTFS_STM32N6570_DK_SD_CARD_DETECT_ACTIVE_LOW ?
        state == GPIO_PIN_RESET : state == GPIO_PIN_SET;
}

static int mtfs_stm32n6570_dk_card_detect_raw(void *opaque)
{
    (void)opaque;
    return HAL_GPIO_ReadPin(MTFS_STM32N6570_DK_SD_CARD_DETECT_PORT,
        MTFS_STM32N6570_DK_SD_CARD_DETECT_PIN) ==
        GPIO_PIN_SET;
}

static uint32_t mtfs_stm32n6570_dk_now_ms(void *opaque)
{
    (void)opaque;
    return HAL_GetTick();
}

static uint64_t mtfs_stm32n6570_dk_divide_u64(
    uint64_t numerator, uint64_t denominator)
{
    uint64_t quotient = 0U;
    uint32_t bit;
    if (denominator == 0U) return UINT64_MAX;
    for (bit = 64U; bit != 0U; --bit) {
        uint32_t shift = bit - 1U;
        if (denominator <= (UINT64_MAX >> shift)) {
            uint64_t shifted = denominator << shift;
            if (shifted <= numerator) {
                numerator -= shifted;
                quotient |= UINT64_C(1) << shift;
            }
        }
    }
    return quotient;
}

uint64_t mtfs_stm32n6570_dk_benchmark_clock_us(void *context)
{
    SYSTIM before = {0};
    SYSTIM after = {0};
    uint32_t reload;
    uint32_t current = 0U;
    uint32_t attempt;
    uint64_t milliseconds;
    uint64_t phase_us = 0U;
    (void)context;

    reload = SysTick->LOAD + 1U;
    for (attempt = 0U; attempt < 4U; ++attempt) {
        (void)tk_get_otm(&before);
        current = SysTick->VAL;
        (void)tk_get_otm(&after);
        if ((before.hi == after.hi) && (before.lo == after.lo) &&
            ((SCB->ICSR & SCB_ICSR_PENDSTSET_Msk) == 0U)) {
            break;
        }
    }
    milliseconds = ((uint64_t)(uint32_t)after.hi << 32U) | after.lo;
    if ((attempt < 4U) && (reload != 0U) && (SystemCoreClock != 0U)) {
        if (current > reload) {
            current = reload;
        }
        phase_us = mtfs_stm32n6570_dk_divide_u64(
            (uint64_t)(reload - current) * UINT64_C(1000000),
            SystemCoreClock);
        if (phase_us >= UINT64_C(10000)) {
            phase_us = UINT64_C(9999);
        }
    }
    return milliseconds * UINT64_C(1000) + phase_us;
}

#if MTFS_ENABLE_STORAGE_SENTINEL
mtfs_error_t mtfs_stm32n6570_dk_sentinel_clock_us(
    void *context, uint64_t *now_us)
{
    if (now_us == NULL) return MTFS_ERROR_INVALID_ARGUMENT;
    *now_us = mtfs_stm32n6570_dk_benchmark_clock_us(context);
    return MTFS_OK;
}
#endif

uint32_t mtfs_stm32n6570_dk_sdmmc_clock_hz(void)
{
    uint32_t source_hz =
        HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SDMMC2);
    uint32_t divider =
        (SDMMC2->CLKCR & SDMMC_CLKCR_CLKDIV) >> SDMMC_CLKCR_CLKDIV_Pos;

    return divider == 0U ? source_hz : source_hz / (2U * divider);
}

static void mtfs_stm32n6570_dk_exti12_handler(UINT interrupt_number)
{
    int rising = __HAL_GPIO_EXTI_GET_RISING_IT(
        MTFS_STM32N6570_DK_SD_CARD_DETECT_PIN) != 0U;
    int falling = __HAL_GPIO_EXTI_GET_FALLING_IT(
        MTFS_STM32N6570_DK_SD_CARD_DETECT_PIN) != 0U;
    int raw_level;
    (void)interrupt_number;

    if (!rising && !falling) {
        return;
    }
    __HAL_GPIO_EXTI_CLEAR_IT(MTFS_STM32N6570_DK_SD_CARD_DETECT_PIN);
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
    /* PN12はactive-lowのため、立上りedgeを即時の取り外し通知に使う。 */
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
    media_config.active_level = MTFS_STM32N6570_DK_SD_CARD_DETECT_ACTIVE_LOW ?
        MTFS_MEDIA_ACTIVE_LOW : MTFS_MEDIA_ACTIVE_HIGH;
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
    __HAL_GPIO_EXTI_CLEAR_IT(MTFS_STM32N6570_DK_SD_CARD_DETECT_PIN);
    interrupt.intatr = TA_HLNG;
    interrupt.inthdr = (FP)mtfs_stm32n6570_dk_exti12_handler;
    service->last_kernel_error = tk_def_int(
        (UINT)MTFS_STM32N6570_DK_SD_CARD_DETECT_IRQ, &interrupt);
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
    HAL_NVIC_SetPriority(MTFS_STM32N6570_DK_SD_CARD_DETECT_IRQ,
        MTFS_STM32N6_CD_IRQ_PRIORITY, 0U);
    HAL_NVIC_EnableIRQ(MTFS_STM32N6570_DK_SD_CARD_DETECT_IRQ);
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
    HAL_NVIC_DisableIRQ(MTFS_STM32N6570_DK_SD_CARD_DETECT_IRQ);
    __HAL_GPIO_EXTI_CLEAR_IT(MTFS_STM32N6570_DK_SD_CARD_DETECT_PIN);
    if (tk_def_int((UINT)MTFS_STM32N6570_DK_SD_CARD_DETECT_IRQ,
            NULL) < E_OK) {
        result = MTFS_ERROR_IO;
    }
    cd_diagnostics.irq_registered = 0U;
    cd_sdmmc = NULL;
    service_result = mtfs_media_service_deinit(service);
    if (service_result != MTFS_OK) {
        /*
         * Retain cd_service as an ownership guard.  Reusing its static stack
         * or media context while the worker may still exist is unsafe.
         * ownership guardとしてcd_serviceを保持する。workerが残る可能性が
         * ある間にstatic stackやmedia contextを再利用してはならない。
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
