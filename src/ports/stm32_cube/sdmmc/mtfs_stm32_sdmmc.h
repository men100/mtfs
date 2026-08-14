/* STM32Cube SDMMC block device for microT-FS. */
#ifndef MTFS_STM32_SDMMC_H
#define MTFS_STM32_SDMMC_H

#include <stdint.h>

#include <tk/tkernel.h>
#ifndef MTFS_STM32_HAL_HEADER
#define MTFS_STM32_HAL_HEADER "stm32n6xx_hal.h"
#endif
#include MTFS_STM32_HAL_HEADER

#include "../../../block/mtfs_block_device.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_STM32_SDMMC_SECTOR_SIZE          (512U)
#define MTFS_STM32_SDMMC_BOUNCE_SECTORS       (8U)
#define MTFS_STM32_SDMMC_BOUNCE_SIZE          \
    (MTFS_STM32_SDMMC_SECTOR_SIZE * MTFS_STM32_SDMMC_BOUNCE_SECTORS)
#define MTFS_STM32_SDMMC_CACHE_LINE_SIZE      (32U)
#define MTFS_STM32_SDMMC_DEFAULT_TIMEOUT_MS   (5000U)

typedef int (*mtfs_stm32_sdmmc_signal_fn)(void *opaque);

typedef struct mtfs_stm32_sdmmc_config
{
    SD_HandleTypeDef *hal_sd;
    IRQn_Type irq_number;
    mtfs_stm32_sdmmc_signal_fn card_present;
    mtfs_stm32_sdmmc_signal_fn write_protected;
    mtfs_stm32_sdmmc_signal_fn idma_platform_ready;
    void *signal_context;
    uint32_t io_timeout_ms;
    uint32_t transfer_timeout_ms;
    uint8_t use_idma;
    uint8_t manage_hal_timebase;
} mtfs_stm32_sdmmc_config_t;

typedef struct mtfs_stm32_sdmmc_diagnostics
{
    uint32_t irq_entries;
    uint32_t rx_complete_callbacks;
    uint32_t tx_complete_callbacks;
    uint32_t error_callbacks;
    uint32_t read_single_starts;
    uint32_t read_multi_starts;
    uint32_t write_single_starts;
    uint32_t write_multi_starts;
    uint32_t read_max_blocks;
    uint32_t write_max_blocks;
    uint32_t aborts;
    uint32_t media_removal_notifications;
    uint32_t media_wait_wakeups;
    uint32_t completion_timeouts;
    uint32_t card_state_timeouts;
    uint32_t last_clkcr;
} mtfs_stm32_sdmmc_diagnostics_t;

/* Concrete by design: applications statically allocate this object. */
typedef struct mtfs_stm32_sdmmc_context
{
    mtfs_stm32_sdmmc_config_t config;
    mtfs_block_device_t block_device;
    mtfs_block_geometry_t geometry;
    ID access_mutex_id;
    ID transfer_event_flag_id;
    volatile uint32_t transfer_hal_error;
    volatile uint8_t transfer_active;
    volatile uint8_t media_removal_pending;
    HAL_StatusTypeDef last_hal_status;
    uint32_t last_hal_error;
    ER last_kernel_error;
    mtfs_error_t last_error;
    mtfs_stm32_sdmmc_diagnostics_t diagnostics;
    uint8_t objects_ready;
    uint8_t irq_registered;
    uint8_t timebase_acquired;
    uint8_t hal_initialized;
    volatile uint8_t initialized;
    uint8_t bounce_buffer[MTFS_STM32_SDMMC_BOUNCE_SIZE]
        __attribute__((aligned(MTFS_STM32_SDMMC_CACHE_LINE_SIZE)));
} mtfs_stm32_sdmmc_context_t;

mtfs_error_t mtfs_stm32_sdmmc_context_init(
    mtfs_stm32_sdmmc_context_t *context,
    const mtfs_stm32_sdmmc_config_t *config);
mtfs_error_t mtfs_stm32_sdmmc_context_deinit(
    mtfs_stm32_sdmmc_context_t *context);
mtfs_block_device_t *mtfs_stm32_sdmmc_block_device(
    mtfs_stm32_sdmmc_context_t *context);
void mtfs_stm32_sdmmc_diagnostics_reset(
    mtfs_stm32_sdmmc_context_t *context);

/*
 * ISR-safe removal hint.  It only invalidates lightweight state and wakes an
 * IDMA waiter. HAL_SD_Abort()/DeInit() remain deferred to normal I/O context.
 * A present notification never restores initialized state.
 */
mtfs_error_t mtfs_stm32_sdmmc_media_changed_isr(
    mtfs_stm32_sdmmc_context_t *context, int present);

/* Global STM32 HAL callbacks; dispatch is restricted to the configured handle. */
void HAL_SD_RxCpltCallback(SD_HandleTypeDef *hal_sd);
void HAL_SD_TxCpltCallback(SD_HandleTypeDef *hal_sd);
void HAL_SD_ErrorCallback(SD_HandleTypeDef *hal_sd);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_STM32_SDMMC_H */
