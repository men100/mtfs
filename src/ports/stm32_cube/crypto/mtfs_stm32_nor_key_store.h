/** @file mtfs_stm32_nor_key_store.h
 * @brief STM32 external-NOR wrapped-key record store adapter. / STM32 external NOR上にwrapped key recordを保存するためのadapter。
 * @details Stores opaque wrapped records in a caller-configured region; it does not define board flash layout or provisioning authority.
 * / 呼び出し側が指定した領域に、内部内容を直接扱わないwrapped key recordを保存する。board側のflash layoutや、key provisioningを行う主体・権限までは定義しない。
 * @ingroup mtfs_ports */
#ifndef MTFS_STM32_NOR_KEY_STORE_H
#define MTFS_STM32_NOR_KEY_STORE_H

/** @addtogroup mtfs_ports
 * @{ */

#include "../../../mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_STM32_NOR_BYTES             (UINT32_C(128) * 1024U * 1024U)
#define MTFS_STM32_NOR_ERASE_BYTES       (UINT32_C(4096))
#define MTFS_STM32_NOR_PROGRAM_BYTES     (UINT32_C(256))
#define MTFS_STM32_NOR_KEY_RESERVED_BYTES (UINT32_C(2) * MTFS_STM32_NOR_ERASE_BYTES)
#define MTFS_STM32_NOR_KEY_OFFSET_A      (MTFS_STM32_NOR_BYTES - MTFS_STM32_NOR_KEY_RESERVED_BYTES)
#define MTFS_STM32_NOR_KEY_OFFSET_B      (MTFS_STM32_NOR_BYTES - MTFS_STM32_NOR_ERASE_BYTES)
#define MTFS_STM32_NOR_KEY_RECORD_BYTES  (UINT32_C(64))
#define MTFS_STM32_NOR_FLEET_KEY_ID      (UINT32_C(1))
#define MTFS_STM32_NOR_FLEET_KEY_VERSION (UINT32_C(1))
#define MTFS_STM32_NOR_WRAPPED_KEY_BYTES  (UINT32_C(32))

typedef union mtfs_stm32_nor_wrapped_key
{
    uint32_t words[MTFS_STM32_NOR_WRAPPED_KEY_BYTES / sizeof(uint32_t)];
    uint8_t bytes[MTFS_STM32_NOR_WRAPPED_KEY_BYTES];
} mtfs_stm32_nor_wrapped_key_t;

typedef int (*mtfs_stm32_nor_read_fn)(void *context, uint32_t offset,
    uint8_t *data, size_t bytes);
typedef int (*mtfs_stm32_nor_erase_fn)(void *context, uint32_t offset);
typedef int (*mtfs_stm32_nor_program_fn)(void *context, uint32_t offset,
    const uint8_t *data, size_t bytes);

typedef struct mtfs_stm32_nor_io
{
    void *context;
    mtfs_stm32_nor_read_fn read;
    mtfs_stm32_nor_erase_fn erase_sector;
    mtfs_stm32_nor_program_fn program;
} mtfs_stm32_nor_io_t;

typedef enum mtfs_stm32_nor_key_store_status
{
    MTFS_STM32_NOR_KEY_STORE_OK = 0,
    MTFS_STM32_NOR_KEY_STORE_NOT_FOUND = 1,
    MTFS_STM32_NOR_KEY_STORE_ALREADY_PROVISIONED = 2,
    MTFS_STM32_NOR_KEY_STORE_INVALID_ARGUMENT = -1,
    MTFS_STM32_NOR_KEY_STORE_INVALID_RECORD = -2,
    MTFS_STM32_NOR_KEY_STORE_IO_ERROR = -3,
    MTFS_STM32_NOR_KEY_STORE_GENERATION_EXHAUSTED = -4
} mtfs_stm32_nor_key_store_status_t;

typedef struct mtfs_stm32_nor_key_metadata
{
    uint32_t generation;
    uint32_t key_id;
    uint32_t key_version;
    uint32_t slot_offset;
} mtfs_stm32_nor_key_metadata_t;

typedef struct mtfs_stm32_nor_key_store_diagnostics
{
    mtfs_stm32_nor_key_store_status_t last_status;
    int32_t last_io_error;
    uint32_t valid_slots;
    uint32_t erase_count;
    uint32_t write_count;
    uint32_t verify_count;
} mtfs_stm32_nor_key_store_diagnostics_t;

mtfs_stm32_nor_key_store_status_t mtfs_stm32_nor_key_store_load(
    const mtfs_stm32_nor_io_t *io,
    mtfs_stm32_nor_wrapped_key_t *wrapped_key,
    mtfs_stm32_nor_key_metadata_t *metadata,
    mtfs_stm32_nor_key_store_diagnostics_t *diagnostics);

mtfs_stm32_nor_key_store_status_t mtfs_stm32_nor_key_store_commit(
    const mtfs_stm32_nor_io_t *io,
    const mtfs_stm32_nor_wrapped_key_t *wrapped_key,
    uint32_t key_id,
    uint32_t key_version,
    int allow_update,
    mtfs_stm32_nor_key_metadata_t *metadata,
    mtfs_stm32_nor_key_store_diagnostics_t *diagnostics);

const char *mtfs_stm32_nor_key_store_status_string(
    mtfs_stm32_nor_key_store_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_ENABLE_SEALED_MODEL */

/** @} */
#endif
