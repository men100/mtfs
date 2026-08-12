/* FatFs mutex adaptation for POSIX hosts. */
#include "../../mtfs_config.h"

#if MTFS_FATFS_MUTEX_ADAPTER == MTFS_FATFS_MUTEX_ADAPTER_POSIX

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <pthread.h>
#include <time.h>

#include "../../fatfs/ffconf.h"

static pthread_mutex_t mtfs_fatfs_mutexes[FF_VOLUMES + 1];
static int mtfs_fatfs_mutex_created[FF_VOLUMES + 1];

static int mtfs_fatfs_mutex_volume_is_valid(int vol)
{
    return (vol >= 0) && (vol <= FF_VOLUMES);
}

int ff_mutex_create(int vol)
{
    if (!mtfs_fatfs_mutex_volume_is_valid(vol) ||
        mtfs_fatfs_mutex_created[vol]) {
        return 0;
    }
    if (pthread_mutex_init(&mtfs_fatfs_mutexes[vol], NULL) != 0) {
        return 0;
    }
    mtfs_fatfs_mutex_created[vol] = 1;
    return 1;
}

void ff_mutex_delete(int vol)
{
    if (!mtfs_fatfs_mutex_volume_is_valid(vol) ||
        !mtfs_fatfs_mutex_created[vol]) {
        return;
    }
    (void)pthread_mutex_destroy(&mtfs_fatfs_mutexes[vol]);
    mtfs_fatfs_mutex_created[vol] = 0;
}

int ff_mutex_take(int vol)
{
    struct timespec deadline;
    time_t seconds;
    long nanoseconds;

    if (!mtfs_fatfs_mutex_volume_is_valid(vol) ||
        !mtfs_fatfs_mutex_created[vol]) {
        return 0;
    }
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return 0;
    }
    seconds = (time_t)(FF_FS_TIMEOUT / 1000);
    nanoseconds = (long)(FF_FS_TIMEOUT % 1000) * 1000000L;
    deadline.tv_sec += seconds;
    deadline.tv_nsec += nanoseconds;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    return pthread_mutex_timedlock(&mtfs_fatfs_mutexes[vol], &deadline) == 0;
}

void ff_mutex_give(int vol)
{
    if (!mtfs_fatfs_mutex_volume_is_valid(vol) ||
        !mtfs_fatfs_mutex_created[vol]) {
        return;
    }
    /* FatFs cannot receive an unlock error. Invalid ownership remains a test bug. */
    (void)pthread_mutex_unlock(&mtfs_fatfs_mutexes[vol]);
}

#else

/* Keep a standards-compliant translation unit without emitting an OS entity. */
typedef int mtfs_fatfs_mutex_posix_adapter_not_selected_t;

#endif /* POSIX adapter selected */
