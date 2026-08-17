/* EK-RA8P1 SD-card SPI block device for microT-FS. */
#ifndef MTFS_RA_SD_SPI_H
#define MTFS_RA_SD_SPI_H

#include <stdint.h>

#include <tk/tkernel.h>
#include "r_ioport_api.h"
#include "r_sci_b_spi.h"

#include "../../../block/mtfs_block_device.h"
#include "../../../block/mtfs_block_diagnostics.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_RA_SD_SPI_SECTOR_SIZE       (512U)
#define MTFS_RA_SD_SPI_DEFAULT_INIT_HZ   (400000U)
#define MTFS_RA_SD_SPI_DEFAULT_DATA_HZ   (4000000U)
#define MTFS_RA_SD_SPI_DEFAULT_TIMEOUT_MS (1000U)
#define MTFS_RA_SD_SPI_DUMMY_SIZE        (514U)
#define MTFS_RA_SD_SPI_DIAGNOSTICS_API_VERSION (UINT16_C(2))
#define MTFS_RA_SD_SPI_DIAGNOSTICS_VALID_ALL (UINT32_MAX)

typedef int (*mtfs_ra_sd_spi_signal_fn)(void *opaque);

typedef enum mtfs_ra_sd_card_type
{
    MTFS_RA_SD_CARD_UNKNOWN = 0,
    MTFS_RA_SD_CARD_SDSC,
    MTFS_RA_SD_CARD_SDHC_SDXC
} mtfs_ra_sd_card_type_t;

typedef enum mtfs_ra_sd_spi_init_stage
{
    MTFS_RA_SD_SPI_INIT_NONE = 0,
    MTFS_RA_SD_SPI_INIT_SPI_OPEN,
    MTFS_RA_SD_SPI_INIT_POWER_UP,
    MTFS_RA_SD_SPI_INIT_CMD0,
    MTFS_RA_SD_SPI_INIT_CMD8,
    MTFS_RA_SD_SPI_INIT_ACMD41,
    MTFS_RA_SD_SPI_INIT_CMD58,
    MTFS_RA_SD_SPI_INIT_CSD,
    MTFS_RA_SD_SPI_INIT_DATA_RATE,
    MTFS_RA_SD_SPI_INIT_COMPLETE
} mtfs_ra_sd_spi_init_stage_t;

typedef struct mtfs_ra_sd_spi_config
{
    const char *device_name;
    const spi_instance_t *spi;
    const ioport_instance_t *ioport;
    bsp_io_port_pin_t chip_select_pin;
    uint32_t initialization_bitrate_hz;
    uint32_t data_bitrate_hz;
    uint32_t initialization_timeout_ms;
    uint32_t transfer_timeout_ms;
    mtfs_ra_sd_spi_signal_fn card_present;
    void *signal_context;
} mtfs_ra_sd_spi_config_t;

typedef struct mtfs_ra_sd_spi_diagnostics
{
    uint16_t api_version;
    uint16_t struct_size;
    uint32_t validity_mask;
    uint32_t reset_epoch;
    uint32_t transfer_starts;
    uint32_t transfer_completions;
    uint32_t transfer_errors;
    uint32_t media_removal_notifications;
    uint32_t media_wait_wakeups;
    uint32_t read_sectors;
    uint32_t write_sectors;
    uint32_t token_wait_calls;
    uint32_t token_poll_bytes;
    uint32_t token_max_polls;
    uint32_t token_timeouts;
    uint32_t ready_wait_calls;
    uint32_t ready_poll_bytes;
    uint32_t ready_max_polls;
    uint32_t ready_timeouts;
    uint32_t acmd41_retries;
    uint32_t monotonic_clock_errors;
    uint32_t cmd0_attempts;
    uint32_t cmd0_no_response;
    uint32_t cmd0_ready_responses;
    uint32_t cmd0_timeouts;
    uint32_t initialization_stage;
    uint32_t card_type;
    uint32_t current_bitrate_hz;
    int32_t last_fsp_error;
    int32_t last_kernel_error;
    int32_t last_error;
    uint8_t last_r1;
    uint8_t initialized;
    uint8_t fsp_open;
    uint8_t media_removal_pending;
} mtfs_ra_sd_spi_diagnostics_t;

/*
 * The object is intentionally concrete so applications can place it in BSS.
 * No heap allocation is performed by this port.
 */
typedef struct mtfs_ra_sd_spi_context
{
    mtfs_ra_sd_spi_config_t config;
    mtfs_block_device_t block_device;

    ID device_descriptor;
    ID device_id;
    ID transfer_event_flag_id;
    ID access_mutex_id;

    spi_cfg_t spi_config_ram;
    sci_b_spi_extended_cfg_t spi_extended_config_ram;

    volatile ER transfer_result;
    volatile fsp_err_t last_fsp_error;
    ER last_kernel_error;
    mtfs_error_t last_error;
    uint8_t last_r1;

    mtfs_ra_sd_card_type_t card_type;
    mtfs_lba_t sector_count;
    uint32_t current_bitrate_hz;
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_ra_sd_spi_diagnostics_t diagnostics;
    mtfs_block_diagnostics_state_t block_diagnostics;
#endif

    uint8_t dummy_tx[MTFS_RA_SD_SPI_DUMMY_SIZE];
    uint8_t registered;
    uint8_t fsp_open;
    volatile uint8_t initialized;
    volatile uint8_t media_removal_pending;
} mtfs_ra_sd_spi_context_t;

/* Apply defaults, create fixed kernel objects, and register device_name. */
mtfs_error_t mtfs_ra_sd_spi_context_init(
    mtfs_ra_sd_spi_context_t *context,
    const mtfs_ra_sd_spi_config_t *config);

/* Close the descriptor/FSP channel and delete kernel objects. */
mtfs_error_t mtfs_ra_sd_spi_context_deinit(mtfs_ra_sd_spi_context_t *context);

mtfs_block_device_t *mtfs_ra_sd_spi_block_device(mtfs_ra_sd_spi_context_t *context);
mtfs_error_t mtfs_ra_sd_spi_diagnostics_get(
    mtfs_ra_sd_spi_context_t *context,
    mtfs_ra_sd_spi_diagnostics_t *snapshot);
mtfs_error_t mtfs_ra_sd_spi_diagnostics_reset(
    mtfs_ra_sd_spi_context_t *context);

/*
 * ISR-safe removal hint.  It only invalidates lightweight state and wakes a
 * transfer waiter.  FSP close/reopen and SD reinitialization stay in normal
 * I/O context.  A present notification never restores initialized state.
 */
mtfs_error_t mtfs_ra_sd_spi_media_changed_isr(
    mtfs_ra_sd_spi_context_t *context, int present);

/* FSP callback selected for the SCI_B SPI stack in configuration.xml. */
void mtfs_ra_sd_spi_callback(spi_callback_args_t *args);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_RA_SD_SPI_H */
