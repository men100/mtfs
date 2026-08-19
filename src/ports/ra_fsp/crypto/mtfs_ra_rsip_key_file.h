#ifndef MTFS_RA_RSIP_KEY_FILE_H
#define MTFS_RA_RSIP_KEY_FILE_H

#include <stdint.h>

#include "mtfs_error.h"
#include "mtfs_wrapped_key_fatfs.h"
#include "r_rsip_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTFS_RA_RSIP_FLEET_KEY_PATH "0:/MTFSKEY.BIN"

typedef struct mtfs_ra_rsip_key_file
{
    uint8_t record[MTFS_WRAPPED_KEY_RECORD_RSIP_AES256_BYTES]
        __attribute__((aligned(16)));
    mtfs_wrapped_key_metadata_t metadata;
    rsip_wrapped_key_t wrapped_key;
    mtfs_wrapped_key_fatfs_diagnostics_t diagnostics;
    mtfs_wrapped_key_fatfs_status_t last_load_status;
    uint8_t loaded;
} mtfs_ra_rsip_key_file_t;

void mtfs_ra_rsip_key_file_init(mtfs_ra_rsip_key_file_t *context);

mtfs_error_t mtfs_ra_rsip_key_file_load(
    mtfs_ra_rsip_key_file_t *context, const char *path);

void mtfs_ra_rsip_key_file_unload(mtfs_ra_rsip_key_file_t *context);

const rsip_wrapped_key_t *mtfs_ra_rsip_key_file_key(
    const mtfs_ra_rsip_key_file_t *context);

#ifdef __cplusplus
}
#endif

#endif /* MTFS_RA_RSIP_KEY_FILE_H */
