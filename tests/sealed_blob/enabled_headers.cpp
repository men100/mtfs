#include "mtfs.h"

static_assert(MTFS_SEALED_BLOB_API_VERSION == 1U, "sealed blob ABI");

int mtfs_sealed_headers_cpp_compile()
{
    mtfs_sealed_blob_t blob{};
    mtfs_sealed_blob_init(&blob);
    return blob.state == MTFS_SEALED_BLOB_CLOSED ? 0 : 1;
}
