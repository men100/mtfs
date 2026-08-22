#include "mtfs.h"

int mtfs_sealed_headers_c_compile(void)
{
    mtfs_sealed_blob_t blob;
    mtfs_sealed_blob_init(&blob);
    return blob.api_version == MTFS_SEALED_BLOB_API_VERSION ? 0 : 1;
}
