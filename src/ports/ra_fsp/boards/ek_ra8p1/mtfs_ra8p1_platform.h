#ifndef MTFS_RA8P1_PLATFORM_H
#define MTFS_RA8P1_PLATFORM_H

#include <stdint.h>

#include "r_external_irq_api.h"
#include "core/mtfs_media.h"
#include "mtfs_media_service.h"
#include "mtfs_ra_sd_spi.h"

typedef struct mtfs_ra8p1_card_detect_diagnostics
{
    uint32_t irq_entries;
    uint32_t rising_edges;
    uint32_t falling_edges;
    uint32_t service_notify_errors;
    uint32_t port_notify_errors;
    uint8_t raw_level;
    uint8_t active_low;
    uint8_t irq_registered;
} mtfs_ra8p1_card_detect_diagnostics_t;

typedef struct mtfs_ra8p1_card_detect_hardware_diagnostics
{
    uint32_t p000_pfs;
    uint32_t p409_pfs;
    uint32_t ielsr;
    uint32_t vector_entry;
    uint32_t expected_vector_entry;
    int32_t vector_number;
    uint8_t irqcr;
    uint8_t nvic_enabled;
    uint8_t nvic_pending;
} mtfs_ra8p1_card_detect_hardware_diagnostics_t;

void mtfs_ra8p1_sd_spi_config(mtfs_ra_sd_spi_config_t *config);

/* 64-bit monotonic microseconds using kernel uptime plus SysTick phase. */
/* kernel uptimeとSysTick位相から64-bit単調増加microsecond値を得る。 */
uint64_t mtfs_ra8p1_benchmark_clock_us(void *context);

#if MTFS_ENABLE_STORAGE_SENTINEL
mtfs_error_t mtfs_ra8p1_sentinel_clock_us(
    void *context, uint64_t *now_us);
#endif

mtfs_error_t mtfs_ra8p1_card_detect_start(
    mtfs_media_context_t *media,
    mtfs_media_service_context_t *service,
    mtfs_ra_sd_spi_context_t *sd_spi,
    mtfs_media_event_fn event_callback,
    void *event_context);
mtfs_error_t mtfs_ra8p1_card_detect_stop(void);
void mtfs_ra8p1_get_card_detect_diagnostics(
    mtfs_ra8p1_card_detect_diagnostics_t *diagnostics);
void mtfs_ra8p1_get_card_detect_hardware_diagnostics(
    mtfs_ra8p1_card_detect_hardware_diagnostics_t *diagnostics);

/* FSP External IRQ callback. It runs in IRQ context and only posts flags. */
/* FSP外部IRQ callback。IRQ contextではflag通知だけを行う。 */
void mtfs_ra8p1_card_detect_callback(external_irq_callback_args_t *args);

#endif /* MTFS_RA8P1_PLATFORM_H */
