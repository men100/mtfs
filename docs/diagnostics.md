# 構造化診断情報API

Phase 3.5では、診断情報を次の3層のキャッシュ済み数値スナップショットとして公開します。

- vendor非依存のBlock Device情報
- removable mediaのlifecycle情報
- RA SD SPIまたはSTM32 SDMMCのtyped snapshot

getterはmedia I/Oを開始せず、動的メモリを確保せず、ファイル名やファイル内容を参照しません。将来のfeature extractorは時刻`t-1`と`t`でsnapshotを採取し、`reset_epoch`または`media_generation`が変わった組を除外したうえで、固定長の差分やrateを計算できます。monotonic timestampは呼出し側が付与します。core snapshotは時計、AI model、推論runtimeへ依存しません。

## 共通Block Device診断

`block/mtfs_block_diagnostics.h`を直接includeするか、`mtfs.h`をincludeします。`mtfs_block_diagnostics_get()`と`mtfs_block_diagnostics_reset()`はdiagnostics無効時にも宣言され、`MTFS_ERROR_NOT_SUPPORTED`を返します。

custom deviceは、静的に確保した`mtfs_block_diagnostics_state_t`を`mtfs_block_diagnostics_attach()`で関連付けることでdiagnosticsへ対応します。attachは`MTFS_BLOCK_CAPABILITY_DIAGNOSTICS`を設定します。proxy deviceは、自身でdiagnostics stateを所有してattachする場合に限って、このcapabilityを引き継いでください。

すべてのcounterは符号なし32-bitで、`UINT32_MAX`に達すると飽和します。この動作は`MTFS_BLOCK_DIAGNOSTICS_FLAG_COUNTERS_SATURATE`で示します。32-bit MCU上で64-bitのread-modify-writeが分断される問題と、その更新costを避けるための選択です。高頻度のsector counterは数日間の連続転送で飽和する可能性があるため、利用側は飽和値を「少なくともこの値」と解釈してください。`reset_epoch`は2^32を法として増加します。2つのsnapshot間で2^32回resetされない限り、resetを識別できます。

### 識別情報と状態フィールド

| フィールド | 意味・単位 |
|---|---|
| `api_version` | 診断契約のversion。現在は1 |
| `struct_size` | producer側snapshotのbyte数。readerは将来のfield追加を許容する |
| `validity_mask` | cached status、geometry、completed sector、timeout counterの有効性 |
| `reset_epoch` | counter resetの識別値。2^32を法とする |
| `flags` | counterの動作。現在は飽和方式 |
| `capabilities` | Block Device capability |
| `status` | 最後に成功したstatus。initialized、media present、write protected |
| `sector_size` | cached logical sector size。単位はbyte |
| `sector_count` | cached logical sector数 |
| `erase_block_size` | cached erase単位。logical sector数 |
| `last_operation` | 最後に実行した共通API operation enum |
| `last_error` | そのoperationの最終`mtfs_error_t` |

`VALID_STATUS`と`VALID_GEOMETRY`は、それぞれ対応する公開APIが成功した後にだけ設定されます。無効なfieldの0は観測値ではなく、値を取得できていないことを意味します。

現在の同期portは要求全体の成功または失敗だけを返すため、`VALID_READ_COMPLETED`と`VALID_WRITE_COMPLETED`を設定します。requested sectorはAPI entryで加算し、completed sectorは`MTFS_OK`の場合に限って要求全体を加算します。部分完了は記録しません。

`mtfs_error_t`にはvendor非依存のtimeout値がないため、version 1では`VALID_TIMEOUT_COUNTER`を設定しません。RA/ST typed snapshotではport固有のtimeout counterを取得できます。

### 累積カウンター

| 分類 | フィールドと計数対象 |
|---|---|
| Initialize | `initialize_calls`, `initialize_successes`, `initialize_failures` |
| Status | `status_calls`, `status_failures` |
| Read | `read_calls`, `read_successes`, `read_failures` |
| Write | `write_calls`, `write_successes`, `write_failures` |
| Sync | `sync_calls`, `sync_successes`, `sync_failures` |
| Geometry | `geometry_calls`, `geometry_failures` |
| Trim | `trim_calls`, `trim_successes`, `trim_failures` |
| Requested sectors | API entryで加算する`read_sectors_requested`, `write_sectors_requested` |
| Completed sectors | 要求全体の成功後に加算する`read_sectors_completed`, `write_sectors_completed` |
| Error classes | `io_errors`, `not_ready_errors`, `no_media_errors`, `write_protected_errors`, `out_of_range_errors`, `timeout_errors`, `other_errors` |

read/write/trim内部のrange checkで実行するgeometry取得は、`geometry_calls`へ加算しません。失敗は呼出し元のread/write/trim operationへ帰属させます。version 1の`timeout_errors`は0かつ無効です。

resetは累積counterをclearし、`reset_epoch`を増加させます。version/size、validity、cached status/geometry/capabilities、last operation、last errorは維持します。hardwareの初期化・終了、error recovery、mount状態やmedia状態の変更、media I/Oは行いません。

## スナップショット整合性と呼出しコンテキスト

get/reset APIはtask context専用です。共通diagnosticsの更新には短いsequence方式を使い、getterは最大8回retryします。更新との競合で安定したcopyを取得できなかった場合は`MTFS_ERROR_NOT_READY`を返します。Block Device APIの既存serialization契約は引き続き適用されます。snapshotは全fieldが完全に同じ瞬間を示すものではなく、operation境界の状態を含む場合があります。

RA/ST getterはportがactiveな間、既存のport access mutexを取得します。lock順序はFatFs volume mutex、port mutexの順です。IRQ/callback counterはmutex保持中にも増える可能性があるため、task fieldに対してbest-effortです。長時間のinterrupt disableは行いません。

mediaのIRQ counterとnotification sequenceもtask側copy中に増える可能性があります。`media_generation`と確定済みmedia stateはmedia task contextで同時に更新します。resetはteardownと直列化してください。IRQとresetが競合した場合、そのIRQはepochの前後どちらかに現れます。

## リムーバブルメディア診断

`mtfs_media_diagnostics_get()`は次を返します。

- version、size、validity、reset epoch
- current media state、stable present
- notification sequence、media generation
- IRQ notification、manual notification、poll check
- debounce start/recheck
- inserted、removed、error event

累積counterは共通diagnosticsと同じく`UINT32_MAX`で飽和します。

`media_generation`はreset対象のcounterではなく、media lifecycleの識別値です。初期化時にmediaが既に存在すれば1、存在しなければ0で開始し、確定したabsent/presentの変化ごとに増加します。counter resetでは維持します。これにより、挿入・抜去を古い観測結果やAI時系列deltaの境界として扱えます。error stateへ入るだけではgenerationを増加させません。

## ポート固有typed snapshot

### RA SD SPI

`mtfs_ra_sd_spi_diagnostics_get/reset()`では次を取得できます。

- transfer start/completion/error
- media removal notification、media wait wakeup
- 完了したread/write sector
- token/readyのcall、poll byte、最大poll、timeout
- ACMD41 retry、monotonic clock error
- CMD0 attempt、no-response、ready-response、timeout
- initialization stage、last R1
- last FSP error、microT-Kernel error、microT-FS error
- card type、current SPI bitrate
- initialized、FSP open、media removal pending

resetはinitialization stageを維持します。getterは現在のstateとerrorをsnapshotへ反映します。

### STM32 SDMMC

`mtfs_stm32_sdmmc_diagnostics_get/reset()`では次を取得できます。

- IRQ、RX/TX/error callback
- single/multi read/write start、最大block数
- abort、media removal、wakeup
- completion timeout、card-state timeout
- last CLKCR、HAL status、HAL ErrorCode、transfer HAL error
- last microT-Kernel error、microT-FS error
- polling/IDMAの選択
- initialized、HAL initialized、transfer active
- cached geometry
- 4096-byte bounce buffer size
- 32-byte cache-line alignment条件

resetはlast CLKCRを維持し、getterは現在のstateとerrorをsnapshotへ反映します。STM32 HAL card infoから信頼できるnegotiated speed modeを取得できないため、推測したmodeは報告しません。`last_clkcr`はperipheral registerのsnapshotであり、card negotiated modeではありません。

RA/ST typed snapshotの累積counterも`UINT32_MAX`で飽和します。maximum fieldは観測した最大値を維持します。`reset_epoch`は共通snapshotと同じく2^32を法として増加します。

## コンパイル時制御とオーバーヘッド

`MTFS_ENABLE_DIAGNOSTICS`の既定値は1で、0または1だけを指定できます。0の場合、common/media/RA/STのcounter更新式とcontext内の診断storageをcompile outします。getter/reset/attach symbolは残り、`MTFS_ERROR_NOT_SUPPORTED`を返します。通常のBlock Device/FatFs semanticsは変わりません。

publicなconcrete contextと`mtfs_block_device_t`のlayoutは設定によって変わるため、source組込みで利用する全objectは同じ設定で一緒にrebuildしてください。

enabled buildのhot pathでは、各公開operation前後の共通bookkeepingと、read/writeのrequested/completed飽和加算が追加されます。既存のtyped low-level counter更新も残ります。浮動小数点演算、latency計測、動的メモリ確保、throughput sampling、media accessは追加しません。

`MTFS_ENABLE_DIAGNOSTICS`以外を同じproject設定にしたRelease full-imageの測定結果は次のとおりです。

| ターゲット | 有効 | 無効 | 有効時のオーバーヘッド |
|---|---:|---:|---:|
| EK-RA8P1 | text 78,380 B; data 0 B; BSS 87,304 B | text 76,044 B; data 0 B; BSS 86,912 B | text 2,336 B; BSS 392 B |
| STM32N6570-DK IDMA | text 79,320 B; data 3,412 B; BSS 90,804 B | text 77,668 B; data 3,412 B; BSS 90,420 B | text 1,652 B; BSS 384 B |

BSS差にはtarget storage context 1個とmedia context 1個が含まれます。32-bit ABI上のcontext差はmedia 64 B、RA 328 B、ST 320 Bです。診断storage単体ではcommon state 192 B、RA typed storage 120 B、ST typed storage 128 Bです。残りはoptionalなdevice pointerとalignment/paddingによるlayout差です。

## コンソールコマンド

RA/ST runner consoleは`diag`、`diag-reset`、`diag-help`、`test-diagnostics-reset`を提供します。文字列整形はrunner側だけで行い、production APIは数値構造体を返します。`diag-reset`は各層のresultを表示し、mount、unmount、initialize、format、storage hardware accessを行いません。

`test-diagnostics-reset`は試験用の新しいSD/media contextを初期化し、公開status/geometry APIでcacheを有効化してから、同一active context内で`raw read -> get -> common/media/port reset -> get -> raw read -> get`を実行します。3層のepoch増加、全累積counterのclear、status/geometry/media generation/target固有stateの維持、同一sector dataの一致、common/port read counterの0からの再増加を検証し、最後にregistry、media service、port contextをcleanupします。raw writeとfilesystem変更は行いません。

```text
> diag
[diag] common ...
[diag] media ...
[mtfs] spi ...       # RA。STではIRQ/callback行
> diag-reset
[diag] reset common=0 media=0 ra=0 (I/O/media state unchanged)
```

## 実機検証状況

### EK-RA8P1

2026-08-16にRelease buildでFatFs roundtrip、microT-Kernel concurrent access、`bench-smoke`、`bench-normal`、RTC/FatFs timestampを実行し、すべてPASSしました。

normal run後のcommon snapshotはread 5,222/5,222/0、write 3,058/3,058/0、sync 155/155/0でした。requested/completed sectorはread 12,838/12,838、write 6,866/6,866で、RA typed snapshotのread/write sector 12,838/6,866と一致しました。SPI transfer starts/completionsはともに11,616,428で、transfer error、token/ready timeout、monotonic clock errorは0でした。

raw 4 KiB readは273.7 KiB/sでした。Phase 3.4 baseline 273.6 KiB/sとの差は約+0.04%です。単発runの変動範囲ですが、diagnostics追加による重大な性能退行は観測されていません。

hotplug enabledのsmoke testでは、idle removal、NO_MEDIA status/read contract、reinsert、再初期化、registry再登録、`fatfs_roundtrip_after_reinsert`がPASSしました。実測snapshotは`media_generation=3`、notification sequence/IRQ 8、debounce 7/2、inserted/removed/error event 1/1/0、RA removal hint 4でした。8回のmechanical bounceが確定event各1回へ収束しています。

hotplug後のcommon readはcall/success/failure 83/82/1、requested/completed sector 88/87でした。RA typed completed read sectorも87です。1回のfailureと1 sectorの差は、抜去後にNO_MEDIAを確認するため意図的に実行したreadであり、requested/completed契約が実機でも機能していることを示します。SPI transfer starts/completionsは112,163で一致し、transfer errorとtimeoutは0でした。

`diag-reset`はcommon/media/RAの全APIで`MTFS_OK`を返しました。直後のsnapshotではcommon/media epochが1へ進み、counterが0になった一方、common status `INITIALIZED|MEDIA_PRESENT`、geometry、last operation/error、media state、`media_generation`、RA initialization stage、last errors、R1、4 MHz bitrateが維持されました。

consoleの`bench-smoke`と`test-fatfs-time`は実行ごとにSD/media contextを新規初期化し、終了時にdeinitします。このため、`diag-reset`後にこれらを実行すると新しいcontextの`reset_epoch=0`と`media_generation=1`で開始します。2026-08-16にRA実機で`test-diagnostics-reset`を実行し、40 checks、0 failuresでPASSしました。reset後は3層のepochが1、common statusが`INITIALIZED|MEDIA_PRESENT`、geometryとmedia generationが維持され、同一contextのraw read成功後にcommon readが1/1/0、requested/completed sectorが1/1、RA read sectorが1へ再増加しました。SPI transfer starts/completionsは124/124、errorとtimeoutは0でした。

### STM32N6570-DK

2026-08-16にRelease buildのIDMA+IRQとpolling fallbackの両方で、smoke、idle hotplug、再挿入後FatFs roundtrip、`bench-smoke`、`bench-normal`、RTC/FatFs timestampを実行し、すべてPASSしました。HAL error、abort、completion/card-state timeoutはいずれも0でした。

IDMAのnormal runではread/writeの最大要求block数が8、IRQ/RX/TXは8,272/5,338/2,934となり、multi-block転送が使用されました。raw readは512 B 826.5 KiB/s、4 KiB 4,047.3 KiB/s、32 KiB 3,837.0 KiB/sでした。Phase 3.4の4 KiB baseline 4,044.0 KiB/sとの差は約+0.08%で、重大な性能退行は観測されていません。

pollingのnormal runではIRQ/RX/TXとmulti-block counterがすべて0、read/writeの最大要求block数が1でした。raw readは512 B 826.4 KiB/s、4 KiB 825.9 KiB/s、32 KiB 816.5 KiB/sでした。Phase 3.4の4 KiB baseline 825.3 KiB/sとの差は約+0.07%です。4 KiBではIDMAがpollingの約4.9倍となり、両modeの実装差がdiagnosticsと性能の両方に反映されています。

hotplug後のsnapshotは、IDMAでcommon read 75/74/1、requested/completed sector 80/79、pollingで77/76/1、82/81でした。各1回のfailureと1 sectorの差は抜去後のNO_MEDIA確認readです。両modeとも`media_generation=3`、inserted/removed/error event 2/1/0となり、再初期化と再挿入後I/Oが成功しました。

console commandは実行ごとにSD/media contextを新規初期化し、終了時にdeinitします。このため、`test-fatfs-time`直後のcommon geometryは、public geometry getterを呼んでいない新しいsnapshotでは未設定値を示し、ST typedのinitialized/HAL initialized/transfer activeは0になります。typed snapshotのcached geometryは維持されており、I/O異常ではありません。

2026-08-16にST IDMA実機で`test-diagnostics-reset`を実行し、40 checks、0 failuresでPASSしました。reset後は3層のepochが1、common status/geometry、media generation、ST geometry/CLKCR/initialized stateが維持されました。同一contextのraw read後はcommon readが1/1/0、requested/completed sectorが1/1、ST read single/maxが1/1、IRQ/RXが1/1へ再増加し、HAL error、abort、timeoutは0でした。polling版はRelease build済みで、同じ実機試験は任意確認として未実施です。
