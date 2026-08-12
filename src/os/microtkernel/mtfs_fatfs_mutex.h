/* FatFs mutex adaptation for microT-Kernel 3.0. */
#ifndef MTFS_FATFS_MUTEX_MICROTKERNEL_H
#define MTFS_FATFS_MUTEX_MICROTKERNEL_H

#include "../../mtfs_config.h"

#if MTFS_FATFS_MUTEX_ADAPTER == MTFS_FATFS_MUTEX_ADAPTER_MICROTKERNEL
#include "../../fatfs/ffconf.h"

int ff_mutex_create(int vol);
void ff_mutex_delete(int vol);
int ff_mutex_take(int vol);
void ff_mutex_give(int vol);
#endif

#endif /* MTFS_FATFS_MUTEX_MICROTKERNEL_H */
