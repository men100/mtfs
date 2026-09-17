/** @file mtfs_stm32_sdmmc.h
 * @brief STM32Cube SDMMC Block Device adapter. / STM32Cube SDMMC向けBlock Device adapter。
 * @details Caller-owned context contains kernel objects and an aligned bounce buffer. IDMA completion uses IRQ callbacks; media_changed_isr only invalidates lightweight state and wakes waiters. HAL abort/deinit remains in task context.
 * / 呼び出し側が所有するcontext内にkernel objectとalignment済みbounce bufferを保持する。IDMAの完了通知にはIRQ callbackを使用する。media_changed_isrでは軽量なstateの無効化とwaiterの起床のみを行い、HALのabort/deinitはtask contextで実行する。
 * @ingroup mtfs_ports */
#ifndef MTFS_STM32_SDMMC_H
#define MTFS_STM32_SDMMC_H

/** @addtogroup mtfs_ports
 * @{ */

#include <stdint.h>

#include <tk/tkernel.h>
#ifndef MTFS_STM32_HAL_HEADER
#define MTFS_STM32_HAL_HEADER "stm32n6xx_hal.h"
#endif
#include MTFS_STM32_HAL_HEADER

#include "../../../block/mtfs_block_device.h"
#include "../../../block/mtfs_block_diagnostics.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_STM32_SDMMC_SECTOR_SIZE          (512U)
#define MTFS_STM32_SDMMC_BOUNCE_SECTORS       (8U)
#define MTFS_STM32_SDMMC_BOUNCE_SIZE          \
    (MTFS_STM32_SDMMC_SECTOR_SIZE * MTFS_STM32_SDMMC_BOUNCE_SECTORS)
#define MTFS_STM32_SDMMC_CACHE_LINE_SIZE      (32U)
#define MTFS_STM32_SDMMC_DEFAULT_TIMEOUT_MS   (5000U)
#define MTFS_STM32_SDMMC_DIAGNOSTICS_API_VERSION (UINT16_C(2))
#define MTFS_STM32_SDMMC_DIAGNOSTICS_VALID_ALL (UINT32_MAX)

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
    uint16_t api_version;
    uint16_t struct_size;
    uint32_t validity_mask;
    uint32_t reset_epoch;
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
    int32_t last_hal_status;
    uint32_t last_hal_error;
    uint32_t transfer_hal_error;
    int32_t last_kernel_error;
    int32_t last_error;
    uint64_t sector_count;
    uint32_t sector_size;
    uint32_t erase_block_size;
    uint32_t bounce_buffer_size;
    uint32_t cache_line_size;
    uint8_t use_idma;
    uint8_t initialized;
    uint8_t hal_initialized;
    uint8_t transfer_active;
    /* IDMA writes that entered post-TX card-ready checking, including fast completion. */
    /* 即時完了した場合も含め、送信完了後のcard-ready確認処理に進んだIDMA writeの回数。 */
    uint32_t ready_sequences;
    /* Calls to the kernel event wait after BUSYD0END was armed. */
    uint32_t ready_event_waits;
    uint32_t busyd0end_irqs;
    uint32_t ready_event_wakeups;
    uint32_t ready_wait_timeouts;
    uint32_t ready_hybrid_fallbacks;
} mtfs_stm32_sdmmc_diagnostics_t;

/* Concrete by design: applications statically allocate this object. */
/* application側で静的確保できるよう、意図的にstruct定義を公開している。 */
typedef struct mtfs_stm32_sdmmc_context
{
    mtfs_stm32_sdmmc_config_t config;
    mtfs_block_device_t block_device;
    mtfs_block_geometry_t geometry;
    ID access_mutex_id;
    ID transfer_event_flag_id;
    volatile uint32_t transfer_hal_error;
    volatile uint8_t transfer_active;
    volatile uint8_t ready_wait_active;
    volatile uint8_t media_removal_pending;
    HAL_StatusTypeDef last_hal_status;
    uint32_t last_hal_error;
    ER last_kernel_error;
    mtfs_error_t last_error;
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_stm32_sdmmc_diagnostics_t diagnostics;
    mtfs_block_diagnostics_state_t block_diagnostics;
#endif
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
mtfs_error_t mtfs_stm32_sdmmc_diagnostics_get(
    mtfs_stm32_sdmmc_context_t *context,
    mtfs_stm32_sdmmc_diagnostics_t *snapshot);
mtfs_error_t mtfs_stm32_sdmmc_diagnostics_reset(
    mtfs_stm32_sdmmc_context_t *context);

/*
 * ISR-safe removal hint.  It only invalidates lightweight state and wakes an
 * IDMA waiter. HAL_SD_Abort()/DeInit() remain deferred to normal I/O context.
 * A present notification never restores initialized state.
 * ISRから安全に呼び出せるmedia取り外し通知。軽量なstateの無効化と
 * IDMA待機中のtaskの起床のみを行い、HAL_SD_Abort()/DeInit()は通常のI/O contextで実行する。
 * present の通知を受けても、initialized 状態には戻さない。
 */
mtfs_error_t mtfs_stm32_sdmmc_media_changed_isr(
    mtfs_stm32_sdmmc_context_t *context, int present);

/* Global STM32 HAL callbacks; dispatch is restricted to the configured handle. */
/* STM32 HALのglobal callback。処理対象はconfigで指定されたhandleに限定する。 */
void HAL_SD_RxCpltCallback(SD_HandleTypeDef *hal_sd);
void HAL_SD_TxCpltCallback(SD_HandleTypeDef *hal_sd);
void HAL_SD_ErrorCallback(SD_HandleTypeDef *hal_sd);

#ifdef __cplusplus
}
#endif

/** @} */
#endif /* MTFS_STM32_SDMMC_H */
