/* microT-FS application configuration entry point. */
#ifndef MTFS_CONFIG_H
#define MTFS_CONFIG_H

/* Versioned diagnostic snapshots and counter collection. */
#ifndef MTFS_ENABLE_DIAGNOSTICS
#define MTFS_ENABLE_DIAGNOSTICS (1)
#endif

/* Maximum number of physical drives in the fixed block-device registry. */
#ifndef MTFS_BLOCK_REGISTRY_SIZE
#define MTFS_BLOCK_REGISTRY_SIZE (4U)
#endif

/* Keep FatFs formatting disabled unless an application explicitly enables it. */
#ifndef MTFS_FF_USE_MKFS
#define MTFS_FF_USE_MKFS (0)
#endif

/* Keep the normal FatFs read/write build unless explicitly configured. */
#ifndef MTFS_FF_FS_READONLY
#define MTFS_FF_FS_READONLY (0)
#endif

/* FatFs volume and timestamp defaults for targets without a calendar RTC. */
#ifndef MTFS_FF_FS_NORTC
#define MTFS_FF_FS_NORTC (1)
#endif

#ifndef MTFS_FF_VOLUMES
#define MTFS_FF_VOLUMES (1)
#endif

/* FatFs synchronization settings. FF_FS_TIMEOUT is milliseconds on Phase 1 ports. */
#ifndef MTFS_FF_FS_REENTRANT
#define MTFS_FF_FS_REENTRANT (0)
#endif

#ifndef MTFS_FF_FS_TIMEOUT
#define MTFS_FF_FS_TIMEOUT (1000)
#endif

#define MTFS_FATFS_MUTEX_ADAPTER_NONE          (0)
#define MTFS_FATFS_MUTEX_ADAPTER_POSIX         (1)
#define MTFS_FATFS_MUTEX_ADAPTER_MICROTKERNEL  (2)

#ifndef MTFS_FATFS_MUTEX_ADAPTER
#define MTFS_FATFS_MUTEX_ADAPTER MTFS_FATFS_MUTEX_ADAPTER_NONE
#endif

#if (MTFS_FF_VOLUMES < 1) || (MTFS_FF_VOLUMES > 10)
#error MTFS_FF_VOLUMES must be between 1 and 10
#endif

#if (MTFS_FF_FS_REENTRANT != 0) && (MTFS_FF_FS_REENTRANT != 1)
#error MTFS_FF_FS_REENTRANT must be 0 or 1
#endif

#if (MTFS_ENABLE_DIAGNOSTICS != 0) && (MTFS_ENABLE_DIAGNOSTICS != 1)
#error MTFS_ENABLE_DIAGNOSTICS must be 0 or 1
#endif

#if (MTFS_FF_FS_NORTC != 0) && (MTFS_FF_FS_NORTC != 1)
#error MTFS_FF_FS_NORTC must be 0 or 1
#endif

#if MTFS_FF_FS_TIMEOUT < 0
#error MTFS_FF_FS_TIMEOUT must not be negative
#endif

#if MTFS_FF_VOLUMES > MTFS_BLOCK_REGISTRY_SIZE
#error MTFS_FF_VOLUMES must not exceed MTFS_BLOCK_REGISTRY_SIZE
#endif

#if (MTFS_FATFS_MUTEX_ADAPTER != MTFS_FATFS_MUTEX_ADAPTER_NONE) && \
    (MTFS_FATFS_MUTEX_ADAPTER != MTFS_FATFS_MUTEX_ADAPTER_POSIX) && \
    (MTFS_FATFS_MUTEX_ADAPTER != MTFS_FATFS_MUTEX_ADAPTER_MICROTKERNEL)
#error MTFS_FATFS_MUTEX_ADAPTER selects an unknown OS adaptation
#endif

#if MTFS_FF_FS_REENTRANT && \
    (MTFS_FATFS_MUTEX_ADAPTER == MTFS_FATFS_MUTEX_ADAPTER_NONE)
#error MTFS_FF_FS_REENTRANT requires a POSIX or microT-Kernel mutex adapter
#endif

#endif /* MTFS_CONFIG_H */
