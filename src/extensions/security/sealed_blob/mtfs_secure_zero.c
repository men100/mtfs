#include "mtfs_secure_zero.h"

#if MTFS_ENABLE_SEALED_MODEL

void mtfs_secure_zero(void *data, size_t size)
{
    volatile unsigned char *cursor = (volatile unsigned char *)data;
    while (cursor != NULL && size != 0U)
    {
        *cursor++ = 0U;
        --size;
    }
}

#else
typedef int mtfs_secure_zero_disabled_translation_unit_t;
#endif /* MTFS_ENABLE_SEALED_MODEL */
