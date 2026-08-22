#include "../../src/mtfs_config.h"

#if MTFS_ENABLE_SEALED_MODEL
#error This compile-only target must keep sealed-model support disabled
#endif

/* These must not pull vendor crypto headers into a FatFs-only build. */
#include "../../src/extensions/security/sealed_blob/mtfs_crypto_provider.h"
#include "../../src/extensions/security/sealed_blob/mtfs_sealed_blob.h"
#include "../../src/extensions/security/sealed_blob/mtfs_sealed_format.h"
#include "../../src/extensions/security/sealed_blob/mtfs_sealed_reader.h"
#include "../../src/extensions/security/sealed_blob/mtfs_secure_zero.h"
#include "../../src/extensions/security/wrapped_key/mtfs_wrapped_key_record.h"
#include "../../src/extensions/security/wrapped_key/mtfs_wrapped_key_fatfs.h"
#include "../../src/ports/ra_fsp/crypto/mtfs_ra8p1_ospi_key_store.h"
#include "../../src/ports/stm32_cube/crypto/mtfs_stm32_saes.h"
#include "../../src/ports/stm32_cube/crypto/mtfs_stm32_nor_key_store.h"
#include "../../src/ports/stm32_cube/crypto/mtfs_stm32n6570_nor.h"

int mtfs_sealed_model_disabled_headers_compile(void)
{
    return MTFS_ENABLE_SEALED_MODEL;
}
