#include "mtfs.h"
#include "fatfs/mtfs_sealed_reader_fatfs.h"

static_assert(MTFS_SEALED_BLOB_API_VERSION == 1U, "sealed blob ABI");

int mtfs_sealed_headers_cpp_compile()
{
    mtfs_sealed_blob_t blob{};
    mtfs_sealed_reader_fatfs_t reader{};
    mtfs_sealed_blob_init(&blob);
    mtfs_sealed_reader_fatfs_init(&reader);
    return blob.state == MTFS_SEALED_BLOB_CLOSED ? 0 : 1;
}
