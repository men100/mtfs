#include "mtfs.h"
#include "extensions/security/sealed_blob/mtfs_crypto_provider.h"
#include "extensions/security/sealed_blob/mtfs_sealed_blob.h"
#include "extensions/security/sealed_blob/mtfs_sealed_format.h"
#include "extensions/security/sealed_blob/mtfs_sealed_reader.h"
#include "extensions/security/sealed_blob/mtfs_secure_zero.h"

int mtfs_sealed_disabled_headers_compile(void)
{
    return MTFS_ENABLE_SEALED_MODEL;
}
