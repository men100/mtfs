# STM32N6570-DK SDMMC2 runner

STM32N6570-DK の SDMMC2 4-bit を microT-FS の `pdrv=0` として使う実機 runner です。既定は SDMMC 内蔵 IDMA + SDMMC2 IRQ、代替は polling です。Phase 3.2ではPN12/EXTI12のCard Detect、edge後だけの500 ms debounce、挿入・抜去・再挿入を追加しました。I-cache/D-cacheを有効のまま使い、RIF readback、raw read、FatFs roundtrip、optional LFN、2 task同時アクセス、IRQ/callback/転送block数を診断します。Phase 3.4ではbasic test後のcommand consoleから非破壊performance benchmarkを実行できます。raw sector writeとformatは行いません。

## 対応ツールとプロジェクト

- STM32CubeIDE 2.1.1
- STM32CubeMX 6.17 系
- STM32Cube FW_N6 V1.3.0
- μT-Kernel BSP2 submodule v1.00.04
- `mtfs_stm32n6570_dk_test_basic.ioc`
- `Appli`（`mtfs_stm32n6570_dk_test_basic_Appli`）: secure LRUN application
- `FSBL`（`mtfs_stm32n6570_dk_test_basic_FSBL`）: basic専用の薄いbuild/debug wrapper

CubeのHAL/CMSIS/ExtMemソースとFSBL実装は`boards/stm32n6570_dk/`で共通管理します。
`Appli/.project`と`FSBL/.project`は相対linked resourceで共通資産を参照し、consumer側の
`FSBL/`にはEclipseメタデータとbasic専用launch設定だけを置きます。`Debug/`、
`Release/`、workspace metadata、`.elf/.bin/.map`は生成物でGit管理外です。

`mtk3_bsp2` v1.00.04 の Armv8-M `interrupt.c` には STM32N657 build typo と RAM vector cache coherence の不足があるため、Appli はその 1 ファイルだけ build exclude し、`application/mtfs_stm32n6570_interrupt_override.c` を使います。submodule 本体や RA target は変更しません。

## CubeIDE import / build

1. repository を submodule 込みで checkout し、`git submodule status` が `1ab52cc5a9f59450e62ab78e76de11f4dd89eb15` であることを確認します。
2. CubeIDEの`File > Import > General > Existing Projects into Workspace`で、
   `tests/targets/stm32n6570_dk/basic`を検索rootにし、親、`Appli`、`FSBL`の3プロジェクトを
   importします。`Copy projects into workspace`は無効にします。
3. `mtfs_stm32n6570_dk_test_basic_Appli`のDebug、次に
   `mtfs_stm32n6570_dk_test_basic_FSBL`のDebugをbuildします。
4. FSBL内の`mtfs_stm32n6570_dk_test_basic Debug`を開始します。この共有launchは
   `Debug/mtfs_stm32n6570_dk_test_basic_Appli.elf`と
   `Debug/mtfs_stm32n6570_dk_test_basic_FSBL.elf`をloadし、`usermain`で停止します。

CubeIDEで`.ioc`を再生成すると`Core/Src/main.c`、`stm32n6xx_hal_msp.c`、`stm32n6xx_it.c`、`.project/.cproject`が更新され得ます。またtest target側に`Drivers`、`Middlewares`、`Secure_nsclib`、`FSBL`が再作成された場合は、必要な生成差分を`boards/stm32n6570_dk/`へ反映し、複製を残さないでください。再生成前後のdiffで次を確認してください。

- USER CODE 内の `HAL_SD_Init` 遅延、pre-kernel RIF、`knl_start_mtkernel()` が残る。
- SDMMC2 global interrupt が enabled、preemption priority 5、subpriority 0。
- PN12 `SD_DETECT` がpull-up、rising/falling EXTI12、preemption priority 6、subpriority 0。
- SDMMC2 は Appli、4-bit、RIF CID1。
- `mtfs_core`、`mtfs_os`、`mtfs_benchmarks`を含むlinked resource/include path、source exclude、`MTFS_*` defineが残り、絶対パスが混入しない。

## IDMA / polling 選択

Appli Debug/Release の C compiler define `MTFS_STM32_SD_USE_IDMA` で切り替えます。

- `1`（既定）: IDMA+IRQ。GPDMA/HPDMA channel は不要です。data pathはSDMMC2 global interrupt、Card Detectは独立したEXTI12を使います。
- `0`: polling fallback。同じ Block Device/FatFs/test API を使いますが、複数 sector の要求を 1 sector ずつの HAL polling 転送へ分割します。SDMMC hardware flow control もこの経路だけ有効にし、速度より確実性を優先します。IDMA 固有の診断 assertion は省略します。

変更後は clean build してください。実機合格は両設定で別々に確認します。

## Phase 3.6 optional LFN

microT-FS共通のrepository既定は`MTFS_FF_USE_LFN=0`ですが、このPhase 3.6 test projectの
Debug/Release既定は次のcompiler defineでASCII長名試験を有効にします。

```text
MTFS_FF_USE_LFN=2
MTFS_FF_MAX_LFN=64
MTFS_FF_CODE_PAGE=437
```

`mtfs_fatfs` source folderでは`ffunicode.c`を除外しません。LFN無効buildを確認する場合は
`MTFS_FF_USE_LFN=0`へ変更してclean buildし、確認後は2へ戻します。mode 2は呼出しtaskの
stackへLFN working bufferを置き、既定64では少なくとも130 byteを加えます。共通concurrent
workerは従来の16 KiB stackを維持し、taskごとの長名と独立`FIL`で同時アクセスします。

runnerは通常roundで`fatfs_lfn`、hotplug再挿入後に`fatfs_lfn_after_reinsert`を実行します。
create/write/sync、rename/readdir、remount、long directory、最大長／異常系、SFN alias衝突、
cleanupがPASSすることを確認します。起動bannerの`LFN=2 max=64 codepage=437`も保存します。
polling smokeは`MTFS_STM32_SD_USE_IDMA=0`とsmoke profileでclean buildし、同じLFN testとcleanupを
確認します。CP437はASCII互換のDOS OEM code pageであり、正式な試験・保証範囲はASCIIの英数字、
space、`-`、`_`、`.`による8.3名とLFNです。CP437拡張文字、CP932/Shift_JIS、日本語filename、
Unicode/UTF-8は正式対応外です。applicationがFatFs APIを直接呼ぶため、独自validation層は追加しません。

## Performance benchmark（Phase 3.4）

通常のbasic testが完了してcommand consoleのpromptが表示された後、次を順に実行します。

```text
bench-info
bench-smoke
bench-normal
```

共通benchmark本体、profile、非破壊条件、ログ項目は
`docs/testing/performance-benchmark.md`に従います。32 KiBの静的bufferを使用し、raw試験は
read-onlyです。FatFs試験が作成・検証・削除するのは`MTFSBEN.TMP`だけで、同名ファイルが
既にあれば上書きせず中止します。

baselineはRelease buildで、IDMA+IRQとpollingをそれぞれclean buildして取得します。
`bench-info`の`mode=idma_irq`または`mode=polling`、4-bit bus、実測SDMMC clock、cache状態を
保存してください。IDMAでは複数block要求をHAL DMAへ保持し、pollingでは1 sectorずつへ分割
するため、両者は別baselineとして扱います。まず`bench-smoke`の全case、pattern検証、6回の
`cleanup file_removed=yes`、`SUITE END status=PASS`を確認してから`bench-normal`を実行します。
各command後のSDMMC診断ではerror、abort、completion/card-state timeoutがすべて0であることも
確認してください。

## RIF、cache、timebase

`mtfs_stm32n6570_dk_pre_kernel_init()` を Cube peripheral setup 後、T-Kernel 起動前に呼びます。SDMMC2 RIMC master index 3 を CID1/Secure/Privileged、SDMMC2 slave を Secure/Privileged に設定し、readback 一致を IDMA の前提条件にします。

IDMA buffer は `mtfs_stm32_sdmmc_context_t` 内の aligned 4096-byte bounce buffer です。Debug map では context が `0x34081480`、buffer が `0x34081520`、linker RAM は `0x34080000` から `0x341fffff` で、32-byte alignment と内部 RAM 配置を満たします。map 値は build ごとに再確認してください。

HAL timeout は microT-Kernel cyclic handler が更新する HAL tick を使います。FatFs は `FF_FS_REENTRANT=1` と microT-Kernel mutex adapter を使用します。lock 順序は FatFs volume mutex が外側、SDMMC port mutex が内側です。

## RTC timestamp provider（Phase 3.3 ST先行実装）

Appliは`MTFS_FF_FS_NORTC=0`、STM32N6 HAL RTC、`src/ports/stm32_cube/rtc`、
`src/core/mtfs_time`、`src/fatfs/mtfs_fattime.c`をリンクします。RTC sourceはSTの
STM32N6570-DK exampleに合わせたLSI（公称32 kHz）です。local timeをそのままFATへ記録し、
timezone/UTC/DST変換は行いません。

設定済みmarkerはTAMP backup register 28-30を予約します。BKP28は`MTFS` magicのcommit
word、BKP29はversion/check、BKP30はmagic反転値です。`set`はcommitを先にclearし、HALで
time/date設定、HAL readback一致確認、BKP29/30、最後にBKP28の順で書きます。marker欠落は
`UNSET`、marker有効かつcalendar不正/read失敗は`ERROR`です。STM32の2桁year制約により
このportのset範囲は2000-2099年です。

RTC/provider mutexはcoordinator task開始時に静的T-Kernel objectとして作成します。通常の
FatFs試験完了後、同じtaskがUSART1 VCP（115200 8N1）でcommand consoleへ移行します。
`MTFS_TARGET_RTC_CONSOLE=1`はこの診断consoleだけを選択し、製品構成では0にできます。
`MTFS_FF_FS_NORTC=1`ではconsole指定にかかわらずRTC初期化とconsoleをコンパイル対象の
実行経路から外し、このtargetのHAL RTC moduleとSTM32 RTC port本体も無効になります。

```text
status
set 2026-08-14 21:30:00
get
test-fatfs-time
bench-info
bench-smoke
bench-normal
clear
help
```

`test-fatfs-time`はSDMMC context、card detect、pdrv 0をその場で初期化し、専用の
`MTFSTIME.TST`を作成・closeして`f_stat()`の日時が作成前後のRTC範囲内にあることを
FATの2秒精度で確認します。最後に専用ファイル、mount、registry、card detect、SDMMC
contextを後片付けします。実行中はカードを抜去しないでください。出力例は次のとおりです。

```text
> test-fatfs-time
[TEST] fatfs_timestamp: PASS (checks=12 failures=0)
RTC before : 2026-08-14 21:35:10
File time  : 2026-08-14 21:35:10
RTC after  : 2026-08-14 21:35:11
[mtfs] FAT timestamp command PASS
```

software system resetではRTCとTAMP backup registerが保持されます。STM32N6 deviceとしては
backup registerはVBAT供給中にVDD-off保持されますが、このtargetはLSIを使うためVDD-off中の
RTC進行を保証しません。電源断保持を必要とする製品boardではLSEと独立VBATを実装し、clock
source変更時はbackup domain/markerを明示的に無効化してください。STM32N6570-DKでの電源断
保持は未実測であり、本Phaseの合否はsoftware reset保持までです。

### RTC実機結果（2026-08-14）

- `set 2026-08-14 21:30:00`後にsoftware system resetし、`status: VALID`を確認。
- reset後の`get`は`21:30:59`、約65秒後は`21:32:04`で、calendar進行とmarker保持を確認。
- 2026-08-16にIDMA/polling両構成で`test-fatfs-time`を実行し、checks 12、failures 0でPASS。

現在はFull Secure imageなのでRTC/TAMP secure aliasへ直接アクセスします。将来TrustZone化
する場合は設定とmarker writeをSecure側へ残し、Non-Secure側には検証済みread serviceだけを
公開します。Cube再生成後はHAL RTC module/source link、`MTFS_FF_FS_NORTC=0`、
`MTFS_TARGET_RTC_CONSOLE`、`mtfs_stm32_rtc`/`mtfs_rtc_set_app` linked resourceを
再確認してください。

## Card Detect構成

socketの`SD_DETECT`はPN12へ接続し、このtargetではLowを挿入、Highを抜去として扱います。
Cube設定はpull-up付き両edge EXTI12です。2026-08-14の実機edgeログで、カードなしHigh、
挿入Low、抜去Highとなるactive-low動作を確認しました。

EXTI12は`tk_def_int(TA_HLNG)`で登録します。ISRはpending clear、raw level、edge/IRQ counter、
共通media serviceとSDMMC portのevent flag通知だけを行います。500 ms後の再読出しと
`INSERTED`/`REMOVED` callbackは静的2048-byte user stackのworker task文脈です。
SDMMC2 IRQ priority 5とEXTI12 priority 6は別handler・別counterです。

既定500 msはSTM32Cube N6のSTM32N6570-DK FileX挿抜例と同じ安定待ちです。実機では1回の
挿入操作で多数の両edgeが観測されることがあるため、各edgeから期限を再設定します。
必要ならAppli compiler define `MTFS_STM32N6_CD_DEBOUNCE_MS=<ms>`で変更できます。

通常runnerでもCard Detect IRQ/serviceは標準で有効です。Appli Debug/Releaseの既定は
normal profile／hotplug offです。対話的挿抜試験時だけ`MTFS_STM32N6570_TEST_PROFILE=1`と
`MTFS_STM32N6570_HOTPLUG_TEST=1`をcompiler defineへ追加します。起動bannerの
`hotplug=on/off`で使用設定を確認できます。

## 実機接続と起動

この runner の Debug は FSBL と Appli を SRAM に load する開発ブート手順です。

1. microSD を取り外し、基板の BOOT switch 2 個を development boot 側（基板正面から右側）へ動かします。
2. ST-LINK USB と VCP terminal（USART1、115200 bps、8 data、no parity、1 stop、flow controlなし）を接続してから電源投入します。
3. CubeIDE の FSBL Debug Configuration から開始し、Appli と FSBL の両 image が download されたことを console で確認します。
4. `BOOT_Application()` 後に Appli の `main()`、microT-Kernel、runner の順で進むことを確認します。

外部 flash 常駐はこの Phase の合否手順ではありません。常駐させる場合は ST signing tool で FSBL/Appli に header を付与し、development boot で NOR external loader `MX66UW1G45G_STM32N6570-DK` を選び、FSBL を `0x70000000`、Appli を `0x70100000` へ program/verify します。その後 BOOT switch 2 個を flash boot 側（左側）へ戻して電源再投入します。未署名の Debug `.bin` をそのまま外部 flash へ書かないでください。

## 実機試験

媒体上の既存ファイルは維持しますが、runnerは`mtfs_phase3.bin`とtask別の一時ファイルを
作成・検証・削除します。書込み可能なFAT12/16/32 microSDを使用し、必要なdataは事前に
backupしてください。mkfsとraw sector writeは行いません。

### Phase 3.1回帰

1. IDMA=1でsmoke、normal、stressを実行し、cache/RIF、geometry、raw read、FatFs、
   concurrent、remount、SDMMC2 IRQ/Rx/Tx診断がPASSすることを確認します。
2. `MTFS_STM32_SD_USE_IDMA=0`へ切り替えてclean buildし、同じprofileを実行します。
   pollingではread/writeの`multi=0`、`max=1`、filesystem結果がIDMAと同じことを確認します。
3. 各構成でpower-cycleし、再起動後もmountとtestが成功することを確認します。

### Phase 3.2挿抜

Appli Debugへ`MTFS_STM32N6570_TEST_PROFILE=1`と
`MTFS_STM32N6570_HOTPLUG_TEST=1`を一時的に追加し、まずIDMA=1でclean buildします。
起動bannerが`profile=smoke rounds=1 ... hotplug=on`であることを確認します。試験後は
両defineを外し、既定の`profile=normal ... hotplug=off`へ戻します。

1. カードなしで起動します。`CD raw=1 active=low`相当でABSENTとなり、runnerが
   `initial ABSENT status PASS`（NO_MEDIA、MEDIA_PRESENT clear）と`card absent: insert`を
   表示してevent待ちになることを確認します。
2. カードを挿入します。EXTI12 falling edge、最後のedgeから500 ms debounce後に`media INSERTED`が1回だけ
   出て、initialize/registerと通常roundtripが成功することを確認します。
3. runnerが`files are closed/synced; remove card while I/O is idle`を表示するまで待ちます。
   ここより前、特にwrite中には抜去しません。
4. mount中かつI/O停止中に抜去します。EXTI12 rising edge、`media REMOVED`、status/readの
   `MTFS_ERROR_NO_MEDIA`、MEDIA_PRESENT clear、unmount/unregisterを確認します。
5. 再挿入します。`INSERTED`後に明示HAL DeInit/Init、registry再登録、
   `fatfs_roundtrip_after_reinsert`がPASSすることを確認します。
6. cleanup後にEXTI12登録、worker task、service/application event flag、SDMMC2 IRQ/objectが
   削除され、次roundまたは再起動で再生成できることを確認します。
7. `MTFS_STM32_SD_USE_IDMA=0`へ切り替え、同じidle挿抜を繰り返してpolling fallbackの
   復旧を確認します。

active read中の抜去は任意試験です。媒体破損riskを了承した専用backup媒体だけを使い、
write、mkfs、raw write中には実施しません。IDMAではremoval eventが転送待ちを起こし、
通常I/O文脈でHAL abortします。polling同期HAL呼出し中は即時abortできず、HAL timeoutが
停止時間の上限です。

期待する診断logはraw CD level、active level、EXTI12 IRQ/rising/falling、debounce start/recheck、
INSERTED/REMOVED/ERROR、media state、mtfs/T-Kernel/HAL error、abort、media wait wakeup、
reinitialize結果です。consoleの`diag`でcommon/media/typed snapshotを取得し、`diag-reset`で
counterだけをresetできます。debuggerではsnapshot取得後に次をwatchします。

- `cd_diagnostics`: EXTI12 entry、rising/falling、raw level、service/port notify error
- `mtfs_media_diagnostics_get()`のsnapshot: debounce、event count、media generation
- `mtfs_stm32_sdmmc_diagnostics_get()`のsnapshot: SDMMC2 IRQ/Rx/Tx、media removal hint/wakeup、timeout/abort
- `sd_context.initialized`、`media_removal_pending`、`last_error`、HAL status/error
- `sd_context.bounce_buffer`: 32-byte alignmentと内部RAM配置
- RIFSC SDMMC2設定、`SCB->CCR`のI/D cache bit、増加中の`uwTick`

### 実機結果（2026-08-14）

STM32N6570-DK、Appli Debug、smoke、IDMA+IRQ、hotplug有効、500 ms debounceで次を確認しました。

- カードなし起動で`MTFS_ERROR_NO_MEDIA`、MEDIA_PRESENT clear。
- PN12/EXTI12で挿入・抜去・再挿入を検出し、active-lowと両edgeを確認。
- INSERTED 2回、REMOVED 1回、ERROR 0回。bounce中のraw edgeは最終levelへ収束。
- 初回initializeと再挿入後の明示initializeが成功。geometryは7,829,504 sector、512 byte。
- raw read、FatFs roundtrip、2-task concurrent、再挿入後roundtripがすべてPASS。
- SDMMC2 IDMA診断はIRQ 102、Rx 61、Tx 41、HAL error/abort/timeout 0。
- idle抜去後のstatus/readがNO_MEDIAとなり、unmount/unregister、service/IRQ/kernel object cleanupが成功。
- I/D cache有効、RIF ready、32-byte aligned内部RAM bounce bufferを維持。

この2026-08-14の結果はidle挿抜のIDMA経路に限定します。polling fallbackは下記のPhase 3.5
実機結果で確認済みです。active read中とwrite中の抜去は未確認で、write中の物理抜去は必須試験に含めません。

## Phase 3.5構造化diagnostics実機確認

Phase 3.5ではcommon Block Device、removable media、STM32 SDMMC typed snapshotを`diag`で表示します。ST typed行にはversion/size/reset epoch/validity、IDMAまたはpolling、initialized/HAL initialized/transfer active、geometry、bounce buffer size、cache-line size/alignmentを出します。HALからcard negotiated speed modeを信頼できる形で取得できないため、`CLKCR`はperipheral register snapshotとしてのみ表示します。

まず既定の`MTFS_STM32_SD_USE_IDMA=1`でRelease clean buildし、次を確認します。

1. `MTFS_STM32N6570_TEST_PROFILE=1`と`MTFS_STM32N6570_HOTPLUG_TEST=1`を一時的に追加し、起動bannerが`Phase 3.5 ... path=IDMA+IRQ hotplug=on`になることを確認します。
2. raw read、FatFs roundtrip、concurrent、`sdmmc_idma_diagnostics`をPASSさせます。typed snapshotではIRQ/RX/TX、read/write multi、最大block数が増え、error callback、abort、completion/card-state timeoutが0であることを確認します。
3. idle removal/reinsertを行い、removal contractと`fatfs_roundtrip_after_reinsert`をPASSさせます。consoleで`diag`を実行し、`media_generation=3`、inserted/removed event、removal hintの増加を記録します。Card Detectのbounce回数と確定event数は一致しなくて構いません。
4. カードを挿入した状態で`test-diagnostics-reset`を実行し、同一active contextで3層のepoch増加、全counter clear、cached state/geometry/media generation/last error/CLKCR維持、reset後raw read、read counter再増加、cleanupがPASSすることを確認します。
5. `bench-smoke`、`bench-normal`、`test-fatfs-time`を実行し、4 KiB raw readをPhase 3.4 IDMA baseline 4044.0 KiB/sと比較します。

次に`MTFS_STM32_SD_USE_IDMA=0`へ変更してRelease clean buildし、同じ試験を繰り返します。typed snapshotのmodeが`polling`、IRQ/RX/TX callbackとmulti-block counterが0、read/write最大block数が1、error/timeoutが0であることを確認します。4 KiB raw readの比較baselineは825.3 KiB/sです。試験後はhotplug/profile defineを外し、使用する既定modeへ戻してclean buildしてください。

### Phase 3.5実機結果（2026-08-16）

STM32N6570-DKのRelease clean buildでIDMA+IRQとpolling fallbackを個別に実行し、次を確認しました。

- 両modeでsmoke、idle removal/reinsert、再挿入後roundtrip、`bench-smoke`、`bench-normal`、`test-fatfs-time`がPASS。
- 両modeでHAL error、abort、completion/card-state timeoutは0。hotplug後は`media_generation=3`、inserted/removed/error eventは2/1/0。
- 抜去後のNO_MEDIA確認により、IDMAはcommon read 75/74/1、requested/completed sector 80/79、pollingは77/76/1、82/81。各1件のfailureは意図した契約確認。
- IDMA normalはIRQ/RX/TX 8,272/5,338/2,934、read/write最大block数8。raw readは512 B 826.5 KiB/s、4 KiB 4,047.3 KiB/s、32 KiB 3,837.0 KiB/s。
- polling normalはIRQ/RX/TX 0/0/0、multi-block 0、read/write最大block数1。raw readは512 B 826.4 KiB/s、4 KiB 825.9 KiB/s、32 KiB 816.5 KiB/s。
- 4 KiB raw readはPhase 3.4 baseline比でIDMA約+0.08%、polling約+0.07%で、diagnostics追加による重大な性能退行は観測されませんでした。IDMAはpollingの約4.9倍です。

console commandは各実行の終了時に対象contextをdeinitします。したがって`test-fatfs-time`直後の`diag`ではST typedのstateが0/0/0となり、public geometry getterを呼んでいないcommon snapshotのgeometryが未設定になる場合があります。typedのcached geometryは512 byte、7,829,504 sector、erase block 1を維持しており、これはI/O異常ではありません。

同一active context用の`test-diagnostics-reset`はIDMA/polling両構成でRelease build済みです。2026-08-16にIDMA実機で実行し、40 checks、0 failuresでPASSしました。reset後はcommon/media/ST epochが1、status/geometry/media generation/CLKCR/initialized stateが維持され、raw read後にcommon requested/completed sectorが1/1、ST read single/maxが1/1、IRQ/RXが1/1へ再増加しました。HAL error、abort、timeoutは0でした。pollingでの同じ実機試験は任意確認として未実施です。

### Phase 3.6実機結果（2026-08-17）

- LFN有効のRelease IDMA+IRQ normal 10周で、8.3 roundtrip、LFN、2-task concurrent、diagnostics、RTC/FatFs timestamp、`bench-smoke`、`bench-normal`がPASSしました。4 KiB raw readはReleaseでPhase 3.4 baselineと同等で、重大な性能退行はありませんでした。
- Release polling fallback smokeでLFN、2-task concurrent、cleanup、`bench-smoke`がPASSしました。IRQ/RX/TXは0、HAL error、abort、completion/card-state timeoutは0でした。
- Release IDMA+IRQ hotplug smokeで、挿入、初期化、LFN、idle抜去、NO_MEDIA contract、再挿入、再初期化、`fatfs_roundtrip_after_reinsert`、`fatfs_lfn_after_reinsert`がPASSしました。inserted/removed/error eventは2/1/0、HAL error、abort、completion/card-state timeoutは0でした。
- LFN無効のRelease buildも成功しました。旧CP932構成での参考値ではLFN有効によるROM増加は約66 KiBで、大半は`ffunicode.c`のDBCS変換tableでした。BSSと既存16 KiB worker stack設定に実質的な増加はありません。
- 既定code pageをCP437へ変更後、Release IDMA+IRQとpollingをclean buildし、どちらもwarning/errorなしで成功しました。通常IDMA構成はtext/data/BSS 91,280 / 3,412 / 90,808 bytes、`ffunicode.o` text 1,190 bytesです。同一hotplug IDMA構成の旧CP932 151,396 / 3,412 / 90,808 bytesに対し、CP437は92,568 / 3,412 / 90,808 bytesで、textを58,828 bytes削減しました。実機normal 10周で起動bannerの`codepage=437`、8.3 roundtrip、LFN 45 checks、2-task concurrent、IDMA diagnosticsを全周PASSし、HAL error、abort、timeoutは0でした。
