/** @file mtfs_ra8p1_ospi_key_store.h
 * @brief EK-RA8P1 OSPI wrapped-key record store adapter. / EK-RA8P1 OSPI上にwrapped key recordを保存するためのadapter。
 * @details Stores only opaque hardware-wrapped records. Flash layout and provisioning policy are supplied by the target project.
 * / hardwareでwrapされ、内部内容を直接扱わないkey recordのみを保存する。flash layoutとkey provisioningの方針はtarget project側で定義する。
 * @ingroup mtfs_ports */
#ifndef MTFS_RA8P1_OSPI_KEY_STORE_H
#define MTFS_RA8P1_OSPI_KEY_STORE_H

/** @addtogroup mtfs_ports
 * @{ */

#include "../../../mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL

#include <stdint.h>

#include "bsp_api.h"
#include "r_rsip_key_injection_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_RA8P1_FLEET_KEY_ID              (UINT32_C(1))
#define MTFS_RA8P1_OSPI_FLASH_BYTES          (UINT32_C(64) * UINT32_C(1024) * UINT32_C(1024))
#define MTFS_RA8P1_OSPI_KEY_SECTOR_BYTES     (UINT32_C(4096))
#define MTFS_RA8P1_OSPI_KEY_RESERVED_BYTES   (UINT32_C(2) * MTFS_RA8P1_OSPI_KEY_SECTOR_BYTES)
#define MTFS_RA8P1_OSPI_KEY_OFFSET_A         (MTFS_RA8P1_OSPI_FLASH_BYTES - MTFS_RA8P1_OSPI_KEY_RESERVED_BYTES)
#define MTFS_RA8P1_OSPI_KEY_OFFSET_B         (MTFS_RA8P1_OSPI_FLASH_BYTES - MTFS_RA8P1_OSPI_KEY_SECTOR_BYTES)
#define MTFS_RA8P1_AES256_WRAPPED_BYTES      (R_RSIP_AES256_KEY_INDEX_WORD_SIZE * sizeof(uint32_t))

typedef enum mtfs_ra8p1_key_store_status
{
    MTFS_RA8P1_KEY_STORE_OK = 0,
    MTFS_RA8P1_KEY_STORE_NOT_FOUND,
    MTFS_RA8P1_KEY_STORE_ALREADY_PROVISIONED,
    MTFS_RA8P1_KEY_STORE_INVALID_ARGUMENT,
    MTFS_RA8P1_KEY_STORE_INVALID_RECORD,
    MTFS_RA8P1_KEY_STORE_IO_ERROR
} mtfs_ra8p1_key_store_status_t;

typedef struct mtfs_ra8p1_key_metadata
{
    uint32_t generation;
    uint32_t key_id;
    uint32_t key_version;
    uint32_t slot_offset;
} mtfs_ra8p1_key_metadata_t;

typedef struct mtfs_ra8p1_key_store_diagnostics
{
    mtfs_ra8p1_key_store_status_t last_status;
    int32_t last_fsp_error;
    uint32_t valid_slots;
    uint32_t erase_count;
    uint32_t write_count;
    uint32_t verify_count;
} mtfs_ra8p1_key_store_diagnostics_t;

mtfs_ra8p1_key_store_status_t mtfs_ra8p1_ospi_key_store_open(
    mtfs_ra8p1_key_store_diagnostics_t *diagnostics);

mtfs_ra8p1_key_store_status_t mtfs_ra8p1_ospi_key_store_load(
    rsip_aes_wrapped_key_t *wrapped_key,
    mtfs_ra8p1_key_metadata_t *metadata,
    mtfs_ra8p1_key_store_diagnostics_t *diagnostics);

mtfs_ra8p1_key_store_status_t mtfs_ra8p1_ospi_key_store_commit(
    const rsip_aes_wrapped_key_t *wrapped_key,
    uint32_t key_id,
    uint32_t key_version,
    int allow_update,
    mtfs_ra8p1_key_metadata_t *metadata,
    mtfs_ra8p1_key_store_diagnostics_t *diagnostics);

void mtfs_ra8p1_ospi_key_store_zero(void *data, uint32_t bytes);

const char *mtfs_ra8p1_key_store_status_string(
    mtfs_ra8p1_key_store_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_ENABLE_SEALED_MODEL */

/** @} */
#endif /* MTFS_RA8P1_OSPI_KEY_STORE_H */
