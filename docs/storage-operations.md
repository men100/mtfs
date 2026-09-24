# Storage operations

## Block deviceとFatFs

`mtfs_block_device_t`は、sector単位のinitialize/status/read/write/sync/geometry/trimに共通のinterfaceを提供します。

registryのslotはFatFsのphysical drive番号に対応します。既定ではregistryのslot数は4、FatFsのvolume数は1です。

read/writeは、指定された`count`個のsectorをすべて転送するか、errorを返します。正常終了した場合に一部のsectorだけが転送されることはありません。

geometryにはsector size、sector count、erase block sizeが含まれます。

FatFsの`diskio` bridgeは、microT-FSのerrorをFatFsのresultへ変換します。

現行のFatFs設定では`FF_USE_TRIM=0`のため、trimは正式に対応するI/O pathではありません。read-onlyは`MTFS_FF_FS_READONLY=1`、format APIは`MTFS_FF_USE_MKFS=1`で明示的に有効化します。

通常applicationではformatを行いません。また、mountに失敗した場合にmediaをformatして自動復旧することもありません。

## SD card hotplug

board ISRはCard Detectのedgeを通知し、task contextでdebounceを行った後、media stateとgenerationを更新します。

cardが取り外されている間のI/Oは`NO_MEDIA`または`NOT_READY`になります。applicationは新しいI/Oの発行を停止し、可能な範囲でopen fileをcloseした後、unmount、unregister、contextのdeinitを行います。

cardを再挿入した後は、initialize、register、mountをあらためて実行します。以前のopen fileが透過的に復旧することはありません。

書き込み中の強制抜去、card内部で完了していないwrite、filesystem破損の回避は保証範囲外です。

hotplug state machineについては[architecture](architecture/io-and-media.md)を参照してください。

## RTCとtimestamp

`mtfs_time_provider_t`は、applicationが所有するlocal calendar time providerです。

`MTFS_FF_FS_NORTC=0`では、`get_fattime()`がVALIDなproviderから取得した時刻をFatFs timestampへ変換します。

providerが未登録の場合、RTCが未設定の場合、またはerrorが発生している場合は、信頼できない時刻を書き込まず`0`を返します。

既定の`MTFS_FF_FS_NORTC=1`では、`2025-01-01`の固定日時を使用します。

consoleを有効にしたtest applicationでは、次のcommandを利用できます。

```text
rtc-status
rtc-get
rtc-set 2026-09-18 12:34:56
rtc-clear
```

初回設定時は、起動後に`rtc-status`で`UNSET`であることを確認し、`rtc-set`で時刻を設定します。

timezone変換、夏時間、時刻同期、backup domain、電池状態の管理はapplication側の責務です。

## Diagnostics

diagnosticsは、common block、media lifecycle、target固有transportの3層に分かれています。target test applicationの`diag` commandでは、これらのsnapshotをまとめて確認できます。

| snapshot            | 公開型とAPI                                                                                                        | 主な確認内容                                                                 |
| ------------------- | -------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------- |
| common block        | `mtfs_block_diagnostics_t`、`mtfs_block_diagnostics_get()` / `mtfs_block_diagnostics_reset()`                   | operationごとの呼び出し/成功/失敗、要求/完了sector数、分類済みerror、status、geometry          |
| media lifecycle     | `mtfs_media_diagnostics_t`、`mtfs_media_diagnostics_get()` / `mtfs_media_diagnostics_reset()`                   | media state、generation、通知、debounce、insert/remove/error event           |
| RA target transport | `mtfs_ra_sd_spi_diagnostics_t`、`mtfs_ra_sd_spi_diagnostics_get()` / `mtfs_ra_sd_spi_diagnostics_reset()`       | SPI transfer、token/ready wait、timeout、FSP/kernel error、初期化stage        |
| ST target transport | `mtfs_stm32_sdmmc_diagnostics_t`、`mtfs_stm32_sdmmc_diagnostics_get()` / `mtfs_stm32_sdmmc_diagnostics_reset()` | SDMMC transfer、IRQ、card-ready wait、timeout、HAL/kernel error、IDMA state |

各snapshotは`api_version`と`struct_size`でlayoutを識別します。fieldを参照する場合は、`validity_mask`で有効とされているものだけを使用してください。

counterには`reset_epoch`以降の累積値が記録され、最大値に達すると飽和してwraparoundしません。common blockの`flags`に含まれる`MTFS_BLOCK_DIAGNOSTICS_FLAG_COUNTERS_SATURATE`は、この性質を示しています。

問題を調査する場合は、まず`last_operation`/`last_error`と分類済みerror counterを確認します。

次に、requested/completed sector数とsuccess/failure数を比較して、どのoperationが正常に完了しなかったかを確認します。そのうえで、media snapshotのstate/generationと、target snapshotのtimeout/driver errorを突き合わせます。

すべてのfieldと定数の定義については[API Reference](https://men100.github.io/mtfs/index.html)を参照してください。

`get`は追加のmedia I/Oを発行せず、保持しているstateからsnapshotを取得します。`reset`はcounterをclearして`reset_epoch`を更新します。

initialized state、geometry、media state、generationなどのcached stateは、reset後も維持されます。

これらの構造化されたdiagnostics情報は、監視、test、AI/NPUへの入力に利用できます。

```text
diag
diag-reset
diag-help
```

## 非破壊performance benchmark

benchmarkのraw pathではsequential readのみを実行し、raw write、trim、formatは行いません。

FatFs pathでは`MTFSBEN.TMP`を`FA_CREATE_NEW`で作成し、deterministic patternのwrite、checksum、readbackによる比較を行います。成功時・失敗時のどちらでもfileをclose/unlinkします。

同名のfileがすでに存在する場合は上書きせず、benchmarkを中止します。

```text
bench-info
bench-smoke
bench-normal
```

request sizeは512 B、4 KiB、32 KiBです。

`end-sync`ではすべてのwriteが完了した後に1回だけ`f_sync()`を実行します。`request-sync`では各writeの直後に`f_sync()`を実行します。

`smoke`ではraw/end-syncを64 KiB、request-syncを32 KiBで実行します。`normal`ではraw/end-syncを1 MiB、request-syncを64 KiBで実行します。

詳細な指標とreference値については[Performance](performance.md)を参照してください。
