/* FatFs mutex adaptation for microT-Kernel 3.0. */
#include "../../mtfs_config.h"

#if MTFS_FATFS_MUTEX_ADAPTER == MTFS_FATFS_MUTEX_ADAPTER_MICROTKERNEL

#include <string.h>

#include <tk/tkernel.h>

#include "mtfs_fatfs_mutex.h"

/* Kernel object IDs are positive; zero is kept as the local invalid value. */
static ID mtfs_fatfs_mutex_ids[FF_VOLUMES + 1];

static int mtfs_fatfs_mutex_volume_is_valid(int vol)
{
    return (vol >= 0) && (vol <= FF_VOLUMES);
}

int ff_mutex_create(int vol)
{
    T_CMTX mutex_attributes;
    ID mutex_id;

    if (!mtfs_fatfs_mutex_volume_is_valid(vol) ||
        (mtfs_fatfs_mutex_ids[vol] > 0)) {
        return 0;
    }

    memset(&mutex_attributes, 0, sizeof(mutex_attributes));
    mutex_attributes.mtxatr = TA_INHERIT;
    mutex_id = tk_cre_mtx(&mutex_attributes);
    if (mutex_id <= 0) {
        return 0;
    }
    mtfs_fatfs_mutex_ids[vol] = mutex_id;
    return 1;
}

void ff_mutex_delete(int vol)
{
    ID mutex_id;

    if (!mtfs_fatfs_mutex_volume_is_valid(vol)) {
        return;
    }
    mutex_id = mtfs_fatfs_mutex_ids[vol];
    if (mutex_id <= 0) {
        return;
    }
    (void)tk_del_mtx(mutex_id);
    mtfs_fatfs_mutex_ids[vol] = 0;
}

int ff_mutex_take(int vol)
{
    ID mutex_id;

    if (!mtfs_fatfs_mutex_volume_is_valid(vol)) {
        return 0;
    }
    mutex_id = mtfs_fatfs_mutex_ids[vol];
    if (mutex_id <= 0) {
        return 0;
    }
    return tk_loc_mtx(mutex_id, (TMO)FF_FS_TIMEOUT) == E_OK;
}

void ff_mutex_give(int vol)
{
    ID mutex_id;

    if (!mtfs_fatfs_mutex_volume_is_valid(vol)) {
        return;
    }
    mutex_id = mtfs_fatfs_mutex_ids[vol];
    if (mutex_id <= 0) {
        return;
    }
    /* FatFs cannot receive an unlock error; leave kernel diagnostics to the target. */
    (void)tk_unl_mtx(mutex_id);
}

#else

/* Keep a standards-compliant translation unit without emitting an OS entity. */
typedef int mtfs_fatfs_mutex_microtkernel_adapter_not_selected_t;

#endif /* microT-Kernel adapter selected */
