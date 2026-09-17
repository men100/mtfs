/** @file mtfs_config.h
 * @brief Compile-time application configuration. / compile時に指定するapplication設定。
 * @details Define overrides before including any microT-FS header and keep the values identical in every translation unit.
 * / 設定を上書きする場合はmicroT-FSのheaderをincludeする前に定義し、すべてのtranslation unitで同じ値を使用すること。
 * @warning Inconsistent feature values across translation units can change public structure layouts.
 * / translation unitごとに設定値が異なると、公開structのlayoutが変わる可能性がある。
 * @ingroup mtfs_core */
#ifndef MTFS_CONFIG_H
#define MTFS_CONFIG_H

/** @addtogroup mtfs_core
 * @{ */

/** Enable versioned diagnostic snapshots and counters. / version情報を含むdiagnostic snapshotとcounterを有効にする。 */
#ifndef MTFS_ENABLE_DIAGNOSTICS
#define MTFS_ENABLE_DIAGNOSTICS (1)
#endif

/** Enable passive Storage Sentinel observation and feature generation. / 追加のI/Oを発生させないStorage Sentinelの観測とfeature生成を有効にする。 */
#ifndef MTFS_ENABLE_STORAGE_SENTINEL
#define MTFS_ENABLE_STORAGE_SENTINEL (0)
#endif

/** Enable heapless fixed-point bundle parsing and inference; requires Storage Sentinel. / heapを使用しない固定小数点bundleの解析と推論を有効にする。Storage Sentinelが必要。 */
#ifndef MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE
#define MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE (0)
#endif

/**
 * Optional sealed-model storage and hardware-crypto integration. Keep this disabled by default so a FatFs-only integration has no crypto dependency.
 * / sealed modelの保存とhardware crypto統合を有効にするためのoptional設定。FatFsのみを使用する構成がcryptoへ依存しないよう、defaultでは無効にする。
 */
#ifndef MTFS_ENABLE_SEALED_MODEL
#define MTFS_ENABLE_SEALED_MODEL (0)
#endif

/** Fixed registry capacity; also bounds valid physical-drive numbers. / registryの固定容量。使用可能なphysical drive番号の上限もこれによって決まる。 */
#ifndef MTFS_BLOCK_REGISTRY_SIZE
#define MTFS_BLOCK_REGISTRY_SIZE (4U)
#endif

/* Keep FatFs formatting disabled unless an application explicitly enables it. */
/* application側で明示的に有効化しない限り、FatFsのformat機能は無効にする。 */
#ifndef MTFS_FF_USE_MKFS
#define MTFS_FF_USE_MKFS (0)
#endif

/* Keep the normal FatFs read/write build unless explicitly configured. */
/* 明示的に変更しない限り、FatFsは通常のread/write可能な構成とする。 */
#ifndef MTFS_FF_FS_READONLY
#define MTFS_FF_FS_READONLY (0)
#endif

/*
 * Optional FatFs long-file-name support. Only the no-LFN configuration and the per-caller stack working buffer are supported by microT-FS. Enabling LFN maps to FatFs FF_USE_LFN=2.
 */
/*
 * FatFsのlong file name（LFN）をoptionalで有効にする。microT-FSが対応するのはLFN無効構成と、callerごとにstack上のwork bufferを使用する構成のみ。LFNを有効にするとFatFsのFF_USE_LFN=2に対応する。
 */
#ifndef MTFS_FF_ENABLE_LFN
#define MTFS_FF_ENABLE_LFN (0)
#endif

#ifndef MTFS_FF_MAX_LFN
#define MTFS_FF_MAX_LFN (64)
#endif

#ifndef MTFS_FF_LFN_UNICODE
#define MTFS_FF_LFN_UNICODE (0)
#endif

#ifndef MTFS_FF_CODE_PAGE
/* CP437 is ASCII-compatible; microT-FS formally supports ASCII names only. */
/* CP437はASCII互換であり、microT-FSが正式にサポートするfile nameはASCIIのみ。 */
#define MTFS_FF_CODE_PAGE 437
#endif

/* FatFs volume and timestamp defaults for targets without a calendar RTC. */
/* calendar RTCを持たないtarget向けのFatFs volume数とtimestamp関連のdefault設定。 */
#ifndef MTFS_FF_FS_NORTC
#define MTFS_FF_FS_NORTC (1)
#endif

#ifndef MTFS_FF_VOLUMES
#define MTFS_FF_VOLUMES (1)
#endif

/* FatFs synchronization settings. FF_FS_TIMEOUT is milliseconds on Phase 1 ports. */
/* FatFsの同期処理に関する設定。Phase 1 portではFF_FS_TIMEOUTの単位はmillisecond。 */
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

#if (MTFS_FF_ENABLE_LFN != 0) && (MTFS_FF_ENABLE_LFN != 1)
#error MTFS_FF_ENABLE_LFN must be 0 or 1
#endif

#if MTFS_FF_ENABLE_LFN && \
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

/** @} */

#endif /* MTFS_CONFIG_H */
