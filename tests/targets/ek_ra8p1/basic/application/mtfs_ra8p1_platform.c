#include "mtfs_ra8p1_platform.h"

#include <string.h>

#include <tk/tkernel.h>

#include "bsp_pin_cfg.h"
#include "hal_data.h"

#ifndef MTFS_RA8P1_CD_ACTIVE_LOW
/* Digilent Pmod MicroSD Rev. A is expected active-low; confirm from raw logs. */
#define MTFS_RA8P1_CD_ACTIVE_LOW (1U)
#endif

#ifndef MTFS_RA8P1_CD_DEBOUNCE_MS
#define MTFS_RA8P1_CD_DEBOUNCE_MS (100U)
#endif

static mtfs_ra8p1_card_detect_diagnostics_t cd_diagnostics;
static mtfs_media_service_context_t *cd_service;
static mtfs_ra_sd_spi_context_t *cd_sd_spi;
static uint8_t cd_irq_open;

static int mtfs_ra8p1_card_detect_raw(void *opaque)
{
    bsp_io_level_t level = BSP_IO_LEVEL_LOW;
    fsp_err_t result;
    (void)opaque;

    result = g_ioport.p_api->pinRead(
        g_ioport.p_ctrl, PMOD2_GPIO1, &level);
    return (result == FSP_SUCCESS) ? (int)level : -1;
}

static int mtfs_ra8p1_card_present(void *opaque)
{
    int raw = mtfs_ra8p1_card_detect_raw(opaque);
    if (raw < 0) {
        return 0;
    }
    return MTFS_RA8P1_CD_ACTIVE_LOW ? !raw : raw;
}

static uint32_t mtfs_ra8p1_now_ms(void *opaque)
{
    SYSTIM time = {0};
    (void)opaque;
    (void)tk_get_tim(&time);
    return time.lo;
}

void mtfs_ra8p1_sd_spi_config(mtfs_ra_sd_spi_config_t *config)
{
    if (config == NULL) {
        return;
    }
    (void)memset(config, 0, sizeof(*config));
    config->device_name = "hspia";
    config->spi = &g_sci_spi0;
    config->ioport = &g_ioport;
    config->chip_select_pin = PMOD2_CTS;
    config->initialization_bitrate_hz = 400000U;
    config->data_bitrate_hz = 4000000U;
    config->initialization_timeout_ms = 1000U;
    config->transfer_timeout_ms = 1000U;
    config->card_present = mtfs_ra8p1_card_present;
}

void mtfs_ra8p1_card_detect_callback(external_irq_callback_args_t *args)
{
    int raw;
    int present;
    uint8_t previous;
    (void)args;

    raw = mtfs_ra8p1_card_detect_raw(NULL);
    if (raw < 0) {
        ++cd_diagnostics.service_notify_errors;
        return;
    }
    previous = cd_diagnostics.raw_level;
    ++cd_diagnostics.irq_entries;
    cd_diagnostics.raw_level = (uint8_t)raw;
    if ((previous == 0U) && raw) {
        ++cd_diagnostics.rising_edges;
    } else if ((previous != 0U) && !raw) {
        ++cd_diagnostics.falling_edges;
    }

    if ((cd_service == NULL) ||
        (mtfs_media_service_notify_isr(cd_service, raw) != MTFS_OK)) {
        ++cd_diagnostics.service_notify_errors;
    }
    present = MTFS_RA8P1_CD_ACTIVE_LOW ? !raw : raw;
    if ((cd_sd_spi != NULL) && !present &&
        (mtfs_ra_sd_spi_media_changed_isr(cd_sd_spi, 0) != MTFS_OK)) {
        ++cd_diagnostics.port_notify_errors;
    }
}

mtfs_error_t mtfs_ra8p1_card_detect_start(
    mtfs_media_context_t *media,
    mtfs_media_service_context_t *service,
    mtfs_ra_sd_spi_context_t *sd_spi,
    mtfs_media_event_fn event_callback,
    void *event_context)
{
    mtfs_media_config_t media_config;
    mtfs_media_service_config_t service_config;
    mtfs_error_t result;
    fsp_err_t fsp_result;
    int raw;

    if ((media == NULL) || (service == NULL) || (sd_spi == NULL) ||
        (cd_service != NULL)) {
        return MTFS_ERROR_INVALID_ARGUMENT;
    }
    raw = mtfs_ra8p1_card_detect_raw(NULL);
    if (raw < 0) {
        return MTFS_ERROR_IO;
    }
    (void)memset(&cd_diagnostics, 0, sizeof(cd_diagnostics));
    cd_diagnostics.active_low = MTFS_RA8P1_CD_ACTIVE_LOW ? 1U : 0U;
    cd_diagnostics.raw_level = (uint8_t)raw;

    (void)memset(&media_config, 0, sizeof(media_config));
    media_config.read_signal = mtfs_ra8p1_card_detect_raw;
    media_config.event_callback = event_callback;
    media_config.event_context = event_context;
    media_config.debounce_ms = MTFS_RA8P1_CD_DEBOUNCE_MS;
    media_config.active_level = MTFS_RA8P1_CD_ACTIVE_LOW
        ? MTFS_MEDIA_ACTIVE_LOW : MTFS_MEDIA_ACTIVE_HIGH;
    result = mtfs_media_init(media, &media_config);
    if (result != MTFS_OK) {
        return result;
    }

    (void)memset(&service_config, 0, sizeof(service_config));
    service_config.media = media;
    service_config.now_ms = mtfs_ra8p1_now_ms;
    service_config.task_priority = 10;
    result = mtfs_media_service_init(service, &service_config);
    if (result != MTFS_OK) {
        (void)mtfs_media_deinit(media);
        return result;
    }

    cd_service = service;
    cd_sd_spi = sd_spi;
    fsp_result = g_sd_card_detect_irq.p_api->open(
        g_sd_card_detect_irq.p_ctrl, g_sd_card_detect_irq.p_cfg);
    if (fsp_result == FSP_SUCCESS) {
        cd_irq_open = 1U;
        fsp_result = g_sd_card_detect_irq.p_api->enable(
            g_sd_card_detect_irq.p_ctrl);
    }
    if (fsp_result != FSP_SUCCESS) {
        if (cd_irq_open) {
            (void)g_sd_card_detect_irq.p_api->close(
                g_sd_card_detect_irq.p_ctrl);
            cd_irq_open = 0U;
        }
        cd_sd_spi = NULL;
        (void)mtfs_media_service_stop_notifications(service);
        if (mtfs_media_service_deinit(service) == MTFS_OK) {
            cd_service = NULL;
            (void)mtfs_media_deinit(media);
        }
        return MTFS_ERROR_NOT_READY;
    }
    cd_diagnostics.irq_registered = 1U;
    return MTFS_OK;
}

mtfs_error_t mtfs_ra8p1_card_detect_stop(void)
{
    mtfs_media_service_context_t *service = cd_service;
    mtfs_media_context_t *media;
    mtfs_error_t result = MTFS_OK;

    if ((service == NULL) || (service->config.media == NULL)) {
        return MTFS_ERROR_NOT_READY;
    }
    media = service->config.media;
    if (mtfs_media_service_stop_notifications(service) != MTFS_OK) {
        result = MTFS_ERROR_IO;
    }
    if (cd_irq_open) {
        if (g_sd_card_detect_irq.p_api->disable(
                g_sd_card_detect_irq.p_ctrl) != FSP_SUCCESS) {
            result = MTFS_ERROR_IO;
        }
        if (g_sd_card_detect_irq.p_api->close(
                g_sd_card_detect_irq.p_ctrl) != FSP_SUCCESS) {
            result = MTFS_ERROR_IO;
        }
        cd_irq_open = 0U;
    }
    cd_diagnostics.irq_registered = 0U;
    cd_sd_spi = NULL;
    if (mtfs_media_service_deinit(service) != MTFS_OK) {
        return MTFS_ERROR_IO;
    }
    cd_service = NULL;
    if (mtfs_media_deinit(media) != MTFS_OK) {
        result = MTFS_ERROR_IO;
    }
    return result;
}

void mtfs_ra8p1_get_card_detect_diagnostics(
    mtfs_ra8p1_card_detect_diagnostics_t *diagnostics)
{
    int raw;
    if (diagnostics == NULL) {
        return;
    }
    *diagnostics = cd_diagnostics;
    raw = mtfs_ra8p1_card_detect_raw(NULL);
    if (raw >= 0) {
        diagnostics->raw_level = (uint8_t)raw;
    }
}
