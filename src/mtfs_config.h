/* microT-FS application configuration entry point. */
#ifndef MTFS_CONFIG_H
#define MTFS_CONFIG_H

/* Versioned diagnostic snapshots and counter collection. */
#ifndef MTFS_ENABLE_DIAGNOSTICS
#define MTFS_ENABLE_DIAGNOSTICS (1)
#endif

/* Optional passive Storage Sentinel observation and feature generation. */
#ifndef MTFS_ENABLE_STORAGE_SENTINEL
#define MTFS_ENABLE_STORAGE_SENTINEL (0)
#endif

/* Optional heapless fixed-point Sentinel bundle parser and CPU inference. */
#ifndef MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE
#define MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE (0)
#endif

/*
 * Optional sealed-model storage and hardware-crypto integration. Keep this
 * disabled by default so a FatFs-only integration has no crypto dependency.
 */
#ifndef MTFS_ENABLE_SEALED_MODEL
#define MTFS_ENABLE_SEALED_MODEL (0)
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

/*
 * Optional FatFs long-file-name support. Only the no-LFN configuration and
 * the per-caller stack working buffer are supported by microT-FS. The maximum
 * is measured in FatFs UTF-16 code units even though the Phase 3.6 API remains
 * ANSI/OEM char strings (not UTF-8).
 */
#ifndef MTFS_FF_USE_LFN
#define MTFS_FF_USE_LFN (0)
#endif

#ifndef MTFS_FF_MAX_LFN
#define MTFS_FF_MAX_LFN (64)
#endif

#ifndef MTFS_FF_LFN_UNICODE
#define MTFS_FF_LFN_UNICODE (0)
#endif

#ifndef MTFS_FF_CODE_PAGE
/* CP437 is ASCII-compatible; microT-FS formally supports ASCII names only. */
#define MTFS_FF_CODE_PAGE 437
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

#if (MTFS_FF_USE_LFN != 0) && (MTFS_FF_USE_LFN != 2)
#error MTFS_FF_USE_LFN must be 0 or 2
#endif

#if MTFS_FF_USE_LFN && \
    ((MTFS_FF_MAX_LFN < 12) || (MTFS_FF_MAX_LFN > 255))
#error MTFS_FF_MAX_LFN must be between 12 and 255 when LFN is enabled
#endif

#if MTFS_FF_LFN_UNICODE != 0
#error MTFS_FF_LFN_UNICODE must be 0; UTF-8/UTF-16/UTF-32 APIs are unsupported
#endif

#if (MTFS_FF_CODE_PAGE != 0) && \
    (MTFS_FF_CODE_PAGE != 437) && (MTFS_FF_CODE_PAGE != 720) && \
    (MTFS_FF_CODE_PAGE != 737) && (MTFS_FF_CODE_PAGE != 771) && \
    (MTFS_FF_CODE_PAGE != 775) && (MTFS_FF_CODE_PAGE != 850) && \
    (MTFS_FF_CODE_PAGE != 852) && (MTFS_FF_CODE_PAGE != 855) && \
    (MTFS_FF_CODE_PAGE != 857) && (MTFS_FF_CODE_PAGE != 860) && \
    (MTFS_FF_CODE_PAGE != 861) && (MTFS_FF_CODE_PAGE != 862) && \
    (MTFS_FF_CODE_PAGE != 863) && (MTFS_FF_CODE_PAGE != 864) && \
    (MTFS_FF_CODE_PAGE != 865) && (MTFS_FF_CODE_PAGE != 866) && \
    (MTFS_FF_CODE_PAGE != 869) && (MTFS_FF_CODE_PAGE != 932) && \
    (MTFS_FF_CODE_PAGE != 936) && (MTFS_FF_CODE_PAGE != 949) && \
    (MTFS_FF_CODE_PAGE != 950)
#error MTFS_FF_CODE_PAGE is not supported by the bundled FatFs
#endif

#if (MTFS_ENABLE_DIAGNOSTICS != 0) && (MTFS_ENABLE_DIAGNOSTICS != 1)
#error MTFS_ENABLE_DIAGNOSTICS must be 0 or 1
#endif

#if (MTFS_ENABLE_STORAGE_SENTINEL != 0) && (MTFS_ENABLE_STORAGE_SENTINEL != 1)
#error MTFS_ENABLE_STORAGE_SENTINEL must be 0 or 1
#endif

#if MTFS_ENABLE_STORAGE_SENTINEL && !MTFS_ENABLE_DIAGNOSTICS
#error MTFS_ENABLE_STORAGE_SENTINEL requires MTFS_ENABLE_DIAGNOSTICS
#endif

#if (MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE != 0) && \
    (MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE != 1)
#error MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE must be 0 or 1
#endif

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE && !MTFS_ENABLE_STORAGE_SENTINEL
#error MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE requires MTFS_ENABLE_STORAGE_SENTINEL
#endif

#if (MTFS_ENABLE_SEALED_MODEL != 0) && (MTFS_ENABLE_SEALED_MODEL != 1)
#error MTFS_ENABLE_SEALED_MODEL must be 0 or 1
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
