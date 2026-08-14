# microT-FS移植ガイド

このガイドは、microT-FSのソースを新しいmicroT-Kernel対応ボードへ取り込み、
Block Device portを実装して実機runnerで確認するまでの入口です。現在の参照実装は
Hostのファイル、RA FSPのSPI接続SD、STM32CubeのSDMMC pollingおよびIDMA + IRQです。
removable mediaの共通設計は[media lifecycle](media-lifecycle.md)を参照してください。

## 読む順序

1. 本章の取り込み手順と接続方式の選択を確認する。
2. [Block Device portの実装契約](block-device-port.md)に従ってportを実装する。
3. [microT-Kernel統合](microtkernel-integration.md)でmutex、IRQ、timebaseを確認する。
4. DMAを使う場合は[DMA/cache coherencyチェックリスト](dma-cache-coherency.md)を適用する。
5. [新規target runnerの作り方](testing-a-new-port.md)に従って試験する。

## ソースの取り込み

microT-FSは事前ビルドした`.a`/`.lib`ではなく、利用する組み込みプロジェクトへ
ソースとヘッダを取り込む構成です。最も単純な配布単位は`src/`全体です。ソースを
絞る場合も、次の共通ファイルは必要です。

```text
src/
  mtfs_config.h
  mtfs_error.h
  mtfs_types.h
  block/
    mtfs_block_device.c/.h
    mtfs_block_registry.c/.h
  fatfs/
    ff.c
    ff.h
    ffconf.h
    diskio.h
    mtfs_diskio.c
  os/microtkernel/
    mtfs_fatfs_mutex.c/.h
  ports/<選択したport>/
```

`ffunicode.c`は現在の`FF_USE_LFN=0`では不要です。`ffsystem.c`も、現在の
microT-Kernel mutex adapterと`FF_USE_LFN=0`の構成では不要です。FatFs設定を変える
場合は`ffconf.h`の条件と依存ソースを改めて確認してください。STM32 SDMMCで
`manage_hal_timebase=1`を使う場合は、`src/ports/stm32_cube/common/`も必要です。

include pathは少なくとも`src`、`src/block`、`src/fatfs`、選択したOS adapter、
選択したportを指定します。さらにportが要求するmicroT-Kernel、CMSIS、FSPまたは
STM32 HALの公開ヘッダへのpathが必要です。使用しないportの`.c`はビルドしません。
特にRA FSPとSTM32Cubeのportを同じtargetへ無条件に追加しないでください。

選択するport sourceは次のとおりです。

| 構成 | 追加するsource |
|---|---|
| Host file | `src/ports/host/mtfs_host_block_file.c` |
| RA FSP SD SPI | `src/ports/ra_fsp/sd_spi/mtfs_ra_sd_spi.c` |
| STM32Cube SDMMC | `src/ports/stm32_cube/sdmmc/mtfs_stm32_sdmmc.c` |
| STM32Cube SDMMCがHAL timebaseも管理 | 上記に加えて`src/ports/stm32_cube/common/mtfs_stm32_hal_timebase.c` |
| 挿抜状態機械 | `src/core/mtfs_media.c` |
| optional microT-Kernel worker | `src/os/microtkernel/mtfs_media_service.c` |

## 基本構成例

次は、read/write可能、1 volume、FatFs再入可能、microT-Kernel mutex、RTCなし、
mkfsなしという現在の実機runnerと同じ設定です。値は`src/mtfs_config.h`に存在する
公開設定名です。IDEの全translation unitにcompile definitionとして与えるか、
プロジェクト管理の設定headerから`mtfs_config.h`より前に定義します。

```c
#define MTFS_FF_FS_READONLY 0
#define MTFS_FF_VOLUMES 1
#define MTFS_FF_FS_REENTRANT 1
#define MTFS_FF_FS_TIMEOUT 1000
#define MTFS_FATFS_MUTEX_ADAPTER MTFS_FATFS_MUTEX_ADAPTER_MICROTKERNEL
#define MTFS_FF_FS_NORTC 1
#define MTFS_FF_USE_MKFS 0
```

`ffconf.h`はこれらを`FF_FS_READONLY`、`FF_VOLUMES`、`FF_FS_REENTRANT`、
`FF_FS_TIMEOUT`、`FF_FS_NORTC`、`FF_USE_MKFS`へ接続します。
`MTFS_FF_VOLUMES`は1から10、かつ`MTFS_BLOCK_REGISTRY_SIZE`以下でなければなりません。
`MTFS_FF_FS_REENTRANT=1`ではmutex adapterの選択が必須です。

`MTFS_FF_FS_NORTC=0`へ変更する場合、現在の共通層は`get_fattime()`を提供しないため、
target/applicationがFatFs契約の`get_fattime()`を実装する必要があります。

## pdrv登録から終了まで

既存target runnerが採る順序は次のとおりです。`context`、
`mtfs_block_device_t`、`FATFS`は利用中に生存する静的領域へ置きます。

```c
/* 1. port固有contextを構成し、Block Deviceを取得する（STM32の例）。 */
error = mtfs_stm32_sdmmc_context_init(&sd_context, &sd_config);
device = mtfs_stm32_sdmmc_block_device(&sd_context);

/* 2. 媒体/HALを初期化してからpdrvへ登録する。 */
error = mtfs_block_initialize(device);
error = mtfs_block_registry_register(0U, device);

/* 3. mountし、FatFs APIを使用する。 */
fat_result = f_mount(&filesystem, "0:", 1U);

/* 4. すべてのFILを閉じてunmountする。 */
fat_result = f_mount(NULL, "0:", 0U);

/* 5. 参照を外してからport固有資源を解放する。 */
error = mtfs_block_registry_unregister(0U);
error = mtfs_stm32_sdmmc_context_deinit(&sd_context);
```

実際の関数名はRAでは`mtfs_ra_sd_spi_context_init()`/
`mtfs_ra_sd_spi_context_deinit()`、STM32では`mtfs_stm32_sdmmc_context_init()`/
`mtfs_stm32_sdmmc_context_deinit()`です。Hostの対応物は
`mtfs_host_block_file_open()`/`mtfs_host_block_file_close()`です。deinitは共通
Block Device operationではありません。unmount前、open中の`FIL`がある間、または
並行I/O中にregistry解除/deinitを行ってはいけません。

`f_mount(..., 1)`は内部でも`disk_initialize()`を要求できます。上の順序はraw I/Oと
geometryをmount前に検証する既存runnerに合わせ、明示的にBlock Deviceを初期化して
います。アプリケーションが初期化をFatFsへ委ねる場合も、先にregistryへ登録し、
portの`initialize`が再実行可能であることを確認してください。

## IDE linked resourceと生成コードの境界

e² studio/CubeIDEでは、リポジトリ相対のlinked resourceで`src/`または必要な
subdirectoryを参照できます。既存runnerは`${workspace_loc:...}`と
`PARENT-n-PROJECT_LOC`を使っています。各開発者のドライブ名やworkspaceの絶対pathを
`.project`、`.cproject`、linker設定へ保存しないでください。プロジェクトを別の深さへ
移すと`PARENT-n-PROJECT_LOC`の`n`も変わるため、import後に解決先を確認します。

CubeMX/FSPが所有するpin、clock、IRQ、HAL handle生成物と、利用者が所有する
microT-FS context、port config、registry/mount処理を分けます。再生成されるファイルへ
変更が必要ならUSER CODE領域を使い、再生成後のdiffで保持されたことを確認します。
microT-FS本体や共通testへvendor HALの型を持ち込まず、target層からport configへ
handle、GPIO callback、IRQ番号、security設定の確認hookを渡します。

## 接続方式の選択

### HostのファイルBlock Device

`src/ports/host/`はファイルをsector配列として扱う開発用参照です。Block Device契約、
Disk I/O bridge、read-only動作、FatFs roundtripをPC上で確認できます。`stdio`と
ファイルseek範囲に依存するためMCU用portではありません。

### SPI接続SD

`src/ports/ra_fsp/sd_spi/`はSCI_B SPI、GPIO CS、FSP callback、microT-Kernel event
flagを一体化したRA固有portです。初期化は80 dummy clocks、CMD0、CMD8、
CMD55/ACMD41、CMD58、CMD9の順で、SDSCはbyte addressing、SDHC/SDXCはblock
addressingです。read/writeはCMD17/CMD24を使用し、Block Deviceの複数sector要求を
内部で1 sectorずつ処理します。DMA/DTCは使いません。

SD protocolとRA FSP transportは現在同じportに含まれます。このファイルを他vendorで
そのままコンパイルできる共通SPI driverとして扱わないでください。新しいvendorでは
同じBlock Device契約に合わせてtransportとprotocolを実装します。

### SDMMC/SDIO

`src/ports/stm32_cube/sdmmc/`はHAL handleとIRQ番号をconfigで受け取ります。
`use_idma=1`は`HAL_SD_ReadBlocks_DMA()`/`HAL_SD_WriteBlocks_DMA()`、SDMMC IRQ、
HAL callback、event flagを使い、4096 byteのbounce buffer単位で最大8 sectorを
multi-block転送します。`use_idma=0`はBlock Device上の複数sector要求を受けますが、
内部では`HAL_SD_ReadBlocks()`/`HAL_SD_WriteBlocks()`を1 sectorずつ呼びます。

card detect、write protect、DMA/RIF準備確認はtarget callbackです。Card Detect IRQは
`mtfs_stm32_sdmmc_media_changed_isr()`へ抜去hintを渡し、IDMA待ちをmedia event bitで
即時解除します。HAL callbackのglobal dispatch制約により、IDMA contextは同時に
1 instanceです。timeout、HAL error、media removalは診断上区別し、通常文脈でabortして
未初期化へ戻し、次の`initialize`でHALをdeinit/initします。

## 現行機能の対応状況

この表は現行の`src/`、設定、既存runner文書から確認できる事実だけを示します。
「未対応」は同梱FatFsに上流機能が存在する場合でも、現在のmicroT-FS設定やportから
利用できないことを意味します。

| 項目 | 分類 | 現在の状態 |
|---|---|---|
| FAT12/FAT16/FAT32 mount/read/write | 共通層で対応済み | FatFs、Disk I/O bridge、Block Device registryで提供。既存実機runnerは事前format済みFAT媒体を使用する。 |
| SDカード挿抜（hot plug） | STM32/RA実機PASS | 共通media層がedge後debounceと重複排除を行う。STM32はIDMA/polling双方、RAはSCI_B SPI+P409/IRQ6でidle抜去後NO_MEDIA、再挿入後の明示initialize/roundtrip、cleanupを確認済み。自動mount、open FIL再開、書込み中抜去のdata保護は保証しない。 |
| card detect | STM32実機確認済み、RA実装済み | STM32N6570-DKはPN12/EXTI12、実測active-low、両edge、priority 6を`tk_def_int(TA_HLNG)`で登録する。RA8P1はPmod Pin 9→P409→FSP ICU IRQ6、両edge、priority 12、実測active-low。P000 IRQ6-DSのISELは競合防止のため無効化する。callback未指定のport契約は常時presentで従来動作を維持する。 |
| write protect | target設定次第 | STM32 portは任意の`write_protected` callbackを持つが、STM32N6570-DK targetはNULL。RAは端子未接続。Hostはopen時のread-only指定とBlock Device capabilityで表現する。 |
| RTC timestamp | target設定次第 | 既定と既存runnerは`MTFS_FF_FS_NORTC=1`で固定日時。0にする場合はtarget/applicationが`get_fattime()`を提供する。共通RTC adapterはない。 |
| LFN/UTF-8 | 未対応 | 現在の`ffconf.h`は`FF_USE_LFN=0`、`FF_LFN_UNICODE=0`。8.3名を使用する。 |
| exFAT | 未対応 | `FF_FS_EXFAT=0`。既存targetはFAT12/16/32のみを対象とする。 |
| multi-volume | target設定次第 | `MTFS_FF_VOLUMES`と固定長registryは複数pdrvを扱える。既定/既存runnerは1 volume。`FF_MULTI_PARTITION=0`なので1物理drive上の任意partition割当は未対応。 |
| trim | 制限あり | 共通Block Device契約と`CTRL_TRIM` bridgeはあるが、`FF_USE_TRIM=0`で、Host/RA/STM32の全portがTRIM capabilityを公開しない。 |
| mkfs | target設定次第 | `MTFS_FF_USE_MKFS`の既定値は0。Host roundtrip targetだけが1でbuildする。既存実機runnerはmkfsせず、事前format済み媒体を使う。 |
| fault recovery | 制限あり | STM32 SDMMCはtimeout/error/media removal時に通常文脈でabort、未初期化化し、再initializeでHAL DeInit/Initする。RA SPIは抜去eventで転送待ちを解除し通常文脈でFSP SPIをclose、再initializeでopenし直す。共通層は検出・通知のみでfilesystem recovery policyは持たない。 |
| atomic file update | 未対応 | atomic replace、journal、transaction用のmicroT-FS共通APIはない。FatFsの通常APIを直接使用する。 |
| read-only構成 | target設定次第 | `MTFS_FF_FS_READONLY=1`と、deviceの`MTFS_BLOCK_CAPABILITY_READ_ONLY`を用途に合わせて設定する。Host compile-only確認がある。 |
| SDMMC IDMA + IRQ | port依存で対応済み | STM32Cube portがHAL callback、event flag、bounce buffer、cache maintenanceを実装する。利用にはtargetのIRQ/RAM/RIF設定が必要。 |

この表は将来の実装方式や優先順位を定めるものではありません。
