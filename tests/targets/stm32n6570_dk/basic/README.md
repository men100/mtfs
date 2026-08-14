# STM32N6570-DK SDMMC2 runner

STM32N6570-DK の SDMMC2 4-bit を microT-FS の `pdrv=0` として使う実機 runner です。既定は SDMMC 内蔵 IDMA + SDMMC2 IRQ、代替は polling です。Phase 3.2ではPN12/EXTI12のCard Detect、edge後だけの500 ms debounce、挿入・抜去・再挿入を追加しました。I-cache/D-cacheを有効のまま使い、RIF readback、raw read、FatFs roundtrip、2 task同時アクセス、IRQ/callback/転送block数を診断します。raw sector writeとformatは行いません。

## 対応ツールとプロジェクト

- STM32CubeIDE 2.1.1
- STM32CubeMX 6.17 系
- STM32Cube FW_N6 V1.3.0
- μT-Kernel BSP2 submodule v1.00.04
- `mtfs_stm32n6570_dk.ioc`
- `FSBL`: First Stage Boot Loader
- `Appli`: secure LRUN application（microT-Kernel と microT-FS はここへリンク）

Cube の HAL/CMSIS/ExtMem ソースは生成プロジェクトの管理対象です。microT-FS、target application、common test、`mtk3_bsp2` は `.project` の相対 linked resource で参照し、コピーを置きません。`Debug/`、`Release/`、workspace metadata、`.elf/.bin/.map` は生成物で Git 管理外です。

`mtk3_bsp2` v1.00.04 の Armv8-M `interrupt.c` には STM32N657 build typo と RAM vector cache coherence の不足があるため、Appli はその 1 ファイルだけ build exclude し、`application/mtfs_stm32n6570_interrupt_override.c` を使います。submodule 本体や RA target は変更しません。

## CubeIDE import / build

1. repository を submodule 込みで checkout し、`git submodule status` が `1ab52cc5a9f59450e62ab78e76de11f4dd89eb15` であることを確認します。
2. CubeIDE の `File > Import > General > Existing Projects into Workspace` で `basic` を root に選び、root、FSBL、Appli を import します。`Copy projects into workspace` は無効にします。
3. `mtfs_stm32n6570_dk_Appli` の Debug、次に `mtfs_stm32n6570_dk_FSBL` の Debug を build します。両方が error/warning 0 であることを確認します。
4. FSBL の Debug Configuration を作り、Startup/Load images で Appli の `Debug/mtfs_stm32n6570_dk_Appli.elf` を download + symbols 対象として追加します。Appli を先に SRAM へ load し、FSBL を load/start する構成にします。

CubeIDE で `.ioc` を再生成すると `Core/Src/main.c`、`stm32n6xx_hal_msp.c`、`stm32n6xx_it.c`、`.project/.cproject` が更新され得ます。再生成前後の diff で次を確認してください。

- USER CODE 内の `HAL_SD_Init` 遅延、pre-kernel RIF、`knl_start_mtkernel()` が残る。
- SDMMC2 global interrupt が enabled、preemption priority 5、subpriority 0。
- PN12 `SD_DETECT` がpull-up、rising/falling EXTI12、preemption priority 6、subpriority 0。
- SDMMC2 は Appli、4-bit、RIF CID1。
- `mtfs_core`、`mtfs_os`を含むlinked resource/include path、source exclude、`MTFS_*` defineが残り、絶対パスが混入しない。

## IDMA / polling 選択

Appli Debug/Release の C compiler define `MTFS_STM32_SD_USE_IDMA` で切り替えます。

- `1`（既定）: IDMA+IRQ。GPDMA/HPDMA channel は不要です。data pathはSDMMC2 global interrupt、Card Detectは独立したEXTI12を使います。
- `0`: polling fallback。同じ Block Device/FatFs/test API を使いますが、複数 sector の要求を 1 sector ずつの HAL polling 転送へ分割します。SDMMC hardware flow control もこの経路だけ有効にし、速度より確実性を優先します。IDMA 固有の診断 assertion は省略します。

変更後は clean build してください。実機合格は両設定で別々に確認します。

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
FatFs試験完了後、同じtaskがUSART1 VCP（115200 8N1）でRTC consoleへ移行します。
`MTFS_TARGET_RTC_CONSOLE=1`はこの診断consoleだけを選択し、製品構成では0にできます。
`MTFS_FF_FS_NORTC=1`ではconsole指定にかかわらずRTC初期化とconsoleをコンパイル対象の
実行経路から外し、このtargetのHAL RTC moduleとSTM32 RTC port本体も無効になります。

```text
status
set 2026-08-14 21:30:00
get
test-fatfs-time
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
- `test-fatfs-time`のSTM32N6570-DK実機実行は未確認。

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
reinitialize結果です。debuggerでは次をwatchします。

- `cd_diagnostics`: EXTI12 entry、rising/falling、raw level、service/port notify error
- `media_context.state`と`media_context.diagnostics`: debounce、event count
- `sd_context.diagnostics`: SDMMC2 IRQ/Rx/Tx、media removal hint/wakeup、timeout/abort
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

この結果はidle挿抜のIDMA経路に限定します。polling fallback、active read中の抜去、write中の
抜去は未確認です。write中の物理抜去は必須試験に含めません。
