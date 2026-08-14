/* FatFs timestamp hook for configurations with a registered RTC provider. */
#include "../mtfs_config.h"

#if !MTFS_FF_FS_READONLY && !MTFS_FF_FS_NORTC

#include "../core/mtfs_time.h"
#include "ff.h"

DWORD get_fattime(void)
{
    mtfs_datetime_t datetime;
    mtfs_time_status_t status;
    uint32_t timestamp;

    if ((mtfs_time_get_local(&datetime, &status) != MTFS_OK) ||
        (status != MTFS_TIME_STATUS_VALID) ||
        (mtfs_datetime_to_fat(&datetime, &timestamp) != MTFS_OK)) {
        return 0U;
    }
    return (DWORD)timestamp;
}

#else

typedef int mtfs_fattime_provider_not_selected_t;

#endif
