#include "mtfs.h"
#include "fatfs/mtfs_sealed_reader_fatfs.h"

int mtfs_sealed_headers_c_compile(void)
{
    mtfs_sealed_blob_t blob;
    mtfs_sealed_reader_fatfs_t reader;
    mtfs_sealed_blob_init(&blob);
    mtfs_sealed_reader_fatfs_init(&reader);
    return blob.api_version == MTFS_SEALED_BLOB_API_VERSION ? 0 : 1;
}
