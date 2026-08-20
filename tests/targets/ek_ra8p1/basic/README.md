# EK-RA8P1 Phase 3.6 RTC / SD SPI / Card Detect / optional LFN runner

Phase 4.1B-RAのRSIP-E50D Compatibility Mode、OSPI鍵保存、console手順は
[`docs/security/ek-ra8p1-rsip-e50d-spike.md`](../../../../docs/security/ek-ra8p1-rsip-e50d-spike.md)
を参照してください。UARTで受信した`K_fleet`をHUK-wrapしてboard上OSPIへ保存するcontest profileは
[`docs/security/ek-ra8p1-ospi-key-provisioning.md`](../../../../docs/security/ek-ra8p1-ospi-key-provisioning.md)
に記録しています。provision、reset後および完全電源断後のOSPI検証、通常版GCM consistency/negative試験は実機PASS済みです。
Phase 4.1B-RA2のfleet固有SD packageによるenvelope/model-key wrap/payload KATと改ざん拒否も実機PASS済みです。
`src/ports/ra_fsp/crypto`はDebug/Release source対象に含めます。

FATで事前フォーマットしたDigilent Pmod MicroSD Revision AをPMOD2へ接続し、microT-FSのBlock Device、FatFs round-trip、microT-Kernel 2タスク並行アクセス、P409/IRQ6による挿入・抜去・再挿入、FSP RTCからFatFs timestampへの反映を確認するe² studioプロジェクトです。テストはカードをフォーマットしません。

## 必要環境

- e² studio 2026-04.2以降
- Renesas RA FSP 6.5.0
- GNU Arm Embedded 13.2.1.arm-13-7
- EK-RA8P1（CPU0）とJ-Link接続
- `mtk3_bsp2` submodule commit `1ab52cc5a9f59450e62ab78e76de11f4dd89eb15`（v1.00.04）

clone後にリポジトリルートで次を実行します。

```sh
git submodule update --init --recursive
```

`mtfs_src`、共通テスト、`mtk3_bsp2` は `.project` の `PARENT-4-PROJECT_LOC` 相対リンクで参照します。絶対パスや本番sourceの複製はありません。`ra/`, `ra_cfg/`, `ra_gen/`, `script/`, `Debug/`, `Release/` はFSP/e² studio生成物なので `.gitignore` 対象です。

指定のBSP2 v1.00.04には、EK-RA8P1の`tm_rcv_dat()`が受信データなしでもreturnする不具合があります。develop branchの修正`6e18f885`を採用するまで、submoduleを変更せずlinker `--wrap`でtarget側実装へ差し替えます。原因、保存patch、削除条件は`docs/adr/0002-ra8p1-tmonitor-receive.md`を参照してください。

## 配線とカード

3.3 V対応microSD SPIモジュールを次のように接続します。詳細と電気条件は `src/ports/ra_fsp/sd_spi/README.md` も参照してください。

| SDモジュール | EK-RA8P1 PMOD2 | MCUピン |
|---|---|---|
| SCK/CLK | PMOD2_RTS_SSL | P601 / SCI0 SCK0 |
| DO/MISO | PMOD2_RX | P602 / SCI0 RXD0 |
| DI/MOSI | PMOD2_TX | P603 / SCI0 TXD0 |
| CS | PMOD2_CTS | P604 / GPIO output high |
| CD / J1 Pin 9 | PMOD2_GPIO1 / J25 Pin 9 | P409 / IRQ6 |
| VCC | 3V3 | 3.3 V |
| GND | GND | GND |

SDカードはPC等でFAT12/FAT16/FAT32のいずれかへ事前フォーマットしてください。exFAT、NTFS、未フォーマット媒体には対応しません。テストはルートへ `RTTEST.BIN` と、LFN有効時は `MicroTFS Inference_Result 2026-08-16.txt` などのPhase 3.6長名およびworker固有の長名、無効時は `TASKA.BIN`, `TASKB.BIN` を一時作成して最後に削除します。同名の既存file/directoryがないカードを使ってください。

## FSP設定

同梱 `configuration.xml` は動作実績のpin/clock条件だけを現在のrunnerへ反映しています。

- SCI_B SPI channel 0、master、mode 0、MSB first
- 初期要求400 kHz（実生成約398,089 Hz）、初期化成功後4 MHzへ再設定
- callback: `mtfs_ra_sd_spi_callback`
- RXI/TXI/TEI/ERI priority 12
- TX/RX transfer instanceはNULL（DMA/DTC未使用）
- P601/P602/P603をSCI0 SCK/RXD/TXD、P604を初期HighのGPIO output
- P409をIRQ mode（IRQ6、input pull-up）、External IRQ channel 6、両edge、priority 12
- 同じ内部IRQ6へ接続されるP000（IRQ6-DS）はGPIO inputのままIRQ inputを無効化
- callback: `mtfs_ra8p1_card_detect_callback`
- RTC `g_rtc0`、Sub-Clock、carry IRQ priority 12
- RTC alarm/periodic IRQとcallbackは未使用
- **Set Source Clock in Open** はDisabled。providerが`VBTBPSR.VBPORF`でバックアップdomain喪失を検出した時だけ`clockSourceSet`を呼ぶ
- RSIP-E50D Compatibility Mode、Arm PSA Crypto、key injection
- OSPI_B unit 0/channel 1、standard SPI。onboard flash末尾8 KiBをHUK-wrapped fleet key専用に予約
- `MTFS_RA8P1_CRYPTO_SPIKE_ENABLE=1`で`crypto-info`、`crypto-consistency`、`crypto-negative`、`crypto-kat`を公開
- flat build専用として`MBEDTLS_PSA_ASSUME_EXCLUSIVE_BUFFERS`を定義し、PSAのwhole-message境界copyを省く

`crypto-kat`と`crypto-negative`はPhase 4.1B-RA2のコマンドです。実際にprovisionした`fleet.key`で
Host生成したSD上の`0:/MTFSKAT.MTF`を使い、OSPI `K_fleet`によるenvelope復号、raw `K_model`の
即時HUK wrap/zeroize、2 payload chunkの既知解、3種類の改ざん拒否を検証します。生成・コピー手順は
[`docs/security/ek-ra8p1-phase-4.1b-ra2.md`](../../../../docs/security/ek-ra8p1-phase-4.1b-ra2.md)
を参照してください。KAT専用の固定profile readerだけを使用し、汎用FatFs model storeはPhase 4.2です。

Card Detectは100 msのsoftware debounceを既定とし、edge後だけoptional media service taskが再確認します。`MTFS_RA8P1_CD_DEBOUNCE_MS`で変更できます。実機で未挿入`raw=1`、挿入`raw=0`を確認済みのため、`MTFS_RA8P1_CD_ACTIVE_LOW=1`を既定としています。

指定のmtk3_bsp2 v1.00.04はRAM例外ベクタのcopy、kernel例外登録、実行中の
`tk_def_int()`更新後にD-cache cleanを行いません。Phase 2.1ではsubmoduleを変更
せず、targetのlinker wrapで各更新範囲をcleanしてDSB/ISBを実行します。通常build
ではI-cache/D-cacheを無効化しません。codeは更新していないためI-cache invalidate
も行いません。根本原因、処理順、上流patch、削除条件は
`docs/adr/0001-ra8p1-vector-cache-coherency.md`を参照してください。

FatFs設定はcompile definitionと `src/mtfs_config.h` により、read/write有効、`FF_FS_REENTRANT=1`、microT-Kernel mutex adapter、`FF_FS_NORTC=0`、1 volume、`FF_USE_MKFS=0`です。`get_fattime()`はVALIDなRTC時刻だけをFAT timestampへ変換し、UNSET/ERROR時は0を返します。

## Phase 3.6 optional LFN

microT-FS共通のrepository既定は`MTFS_FF_USE_LFN=0`ですが、このPhase 3.6 test projectの
Debug/Release既定は次のcompile definitionでASCII長名試験を有効にします。

```text
MTFS_FF_USE_LFN=2
MTFS_FF_MAX_LFN=64
MTFS_FF_CODE_PAGE=437
```

`mtfs_src` source folderでは`ffunicode.c`を除外しません。LFN無効buildを確認する場合は
`MTFS_FF_USE_LFN=0`へ変更してclean buildし、確認後は2へ戻します。mode 2は呼出しtaskの
stackへLFN working bufferを置き、既定64では少なくとも130 byteを加えます。coordinatorと
並行test workerは従来の16 KiB stackを維持し、workerごとの長名と独立`FIL`で同時アクセスします。

runnerは通常roundで`fatfs_lfn`、hotplug再挿入後に`fatfs_lfn_after_reinsert`を実行します。
create/write/sync、rename/readdir、remount、long directory、最大長／異常系、SFN alias衝突、
cleanupがPASSすることを確認します。起動bannerの`LFN=2 max=64 codepage=437`も保存します。
CP437はASCII互換のDOS OEM code pageであり、正式な試験・保証範囲はASCIIの英数字、space、
`-`、`_`、`.`による8.3名とLFNです。CP437拡張文字、CP932/Shift_JIS、日本語filename、
Unicode/UTF-8は正式対応外です。applicationがFatFs APIを直接呼ぶため、独自validation層は追加しません。

旧CP932構成（2026-08-17、FSP 6.5.0／Arm GCC 13.2.1）のRelease clean buildでは、LFN有効が
text 150,340 bytes、BSS 87,312 bytes、無効がtext 82,596 bytes、BSS 87,304 bytesでした。
差分はtext +67,744 bytes、BSS +8 bytesで、`ffunicode.o`のtext 60,134 bytesが大半です。
同じ通常Release構成をCP437へ変更するとtext 91,836 bytes、data 0 bytes、BSS 87,312 bytes、
`ffunicode.o` text 1,210 bytesとなり、CP932比でtext 58,504 bytesを削減しました。
`-fstack-usage`による静的値は`test_fatfs_lfn()` 4,480 bytes、coordinator 328 bytes、
worker task 56 bytesで、各16 KiB task stackを維持できます。実機でのhigh-water markでは
ないため、将来`MTFS_FF_MAX_LFN`やtest local bufferを増やす場合は再測定してください。

## ビルド

1. e² studioで **File > Import > General > Existing Projects into Workspace** を選び、この `basic` directoryを指定します。
2. `configuration.xml` を開き、FSP 6.5.0 packが選択されていることを確認します。
3. 必要なら **Generate Project Content** を実行します。
4. configurationを **Debug** にして **Project > Build Project** を実行します。

Debug/Releaseのcompiler warning基準はともに`-Wall`です。Releaseは最適化が`-O2`、Debugは
`-O0`という違いがあります。ReleaseだけでmicroT-Kernelへ大量に出ていた
`-Wconversion`、`-Wcast-function-type`などの追加診断は構成間の差だったため有効化しません。
target applicationを厳格診断する場合は、依存するmicroT-Kernelとは分けて実施します。

成功時は `Debug/mtfs_ek_ra8p1_basic.elf` と `.srec` が生成されます。Phase 3.3の
Debug build確認値はtext 90,840 bytes、BSS 54,204 bytesです。coordinatorと並行test workerに
加え、明示的に組み込んだmedia serviceの2 KiB static stackを含みます。

## Phase 3.4 performance benchmark

Release buildを書き込み、既存test run後のconsoleで `bench-info`、`bench-smoke`、`bench-normal` を実行できます。benchmarkはraw readだけを行い、FatFsでは8.3名の `MTFSBEN.TMP` だけを `FA_CREATE_NEW` で作成します。同名ファイルがあれば中止し、formatは行いません。詳細なprofile条件、指標、ログ保存項目は `../../../../docs/testing/performance-benchmark.md` を参照してください。

RA baselineの取得前に `bench-smoke` が `SUITE END status=PASS`、6つの `cleanup file_removed=yes`、最後の `COMMAND status=PASS` を出すことを確認してください。その後、同じカードとbuildのまま `bench-normal` を1回実行し、console出力全体を保存します。

Phase 3.4aではSD data token/write ready待ちのpoll単位task delayを除去し、`tk_get_otm()`の実時間deadlineへ変更しています。benchmark終了時に表示される`[mtfs] wait token ...`、`[mtfs] wait ready ...`、`[mtfs] init ...`も保存し、timeoutとmonotonic clock errorが0であることを確認してください。kernel tickは引き続き10 ms、SPI data clockは4 MHzで、Phase 3.4のprofileや非破壊条件は変更していません。

CMD0前にはSD power-up条件を満たすsettle待ちを入れ、無応答`0xFF`とready response `0x00`を1秒の実時間deadline内で再試行します。`[mtfs] init stage=... cmd0_attempts=... no_response=... ready_response=... timeouts=...`を出力するため、再現性の低い初期化失敗でもstageを特定できます。正常終了は`stage=complete`かつ`timeouts=0`です。R1のerror bitは即座にI/O errorとして扱い、card detectで不在を確認した場合だけno-mediaとします。

### 反復profileとfallback

既定はnormal 10周です。e² studioの **C/C++ Build > Settings > GNU Arm Cross C
Compiler > Preprocessor** で次のcompile definitionを追加すると切り替えられます。

| profile | definition | 周回数 |
|---|---|---:|
| smoke | `MTFS_RA8P1_TEST_PROFILE=1` | 1 |
| normal | なし、または`=2` | 10 |
| stress | `MTFS_RA8P1_TEST_PROFILE=3` | 100 |

挿抜試験は既定で無効です。実機smoke試験ではcompiler defineへ次を追加します。

```text
MTFS_RA8P1_TEST_PROFILE=1
MTFS_RA8P1_HOTPLUG_TEST=1
```

各周でSD context/init、geometry、sector 0 read、mount/unmount、file round-trip、
2-task並行access、remount後検証、file削除、task/event flag/context解放まで行います。
mkfsは呼びません。sector 0末尾の`55 AA`は表示だけで合否条件ではありません。

診断時だけ`MTFS_RA8P1_DISABLE_CACHES_FALLBACK=1`を定義すると旧来の全面cache
無効化を再現できます。このbuildは起動logに`fallback=ACTIVE`を出し、Phase 3.2の
合格対象にはなりません。通常buildにはこのdefinitionを設定しないでください。

## 書込みと実行

1. ボードのPMOD2へ電源OFF状態で配線し、FATカードを挿入します。
2. e² studioでEK-RA8P1 / J-LinkのRenesas GDB Hardware Debug構成を作り、`Debug/mtfs_ek_ra8p1_basic.elf` を指定します。
3. download後にCPUをresumeします。
4. e² studio Debug Virtual Console（`tm_printf`出力）を確認します。

実機試験はSDへ書込みを行います。重要データのないカードで実施してください。runnerが`ACTION REQUIRED`を表示するまでは抜き差しせず、active write中の抜去は行わないでください。待機中は5秒ごとにGPIO raw levelとIRQ回数を表示します。raw levelが期待値へ変化したのにIRQ回数が500 ms変化しない場合は、ICU/NVIC/VTOR診断を出して早期FAILします。

### SW1 boot override

電源投入時、またはRESET解除時にSW1を押しておくと、`usermain()`開始時の押下を
ラッチし、自動Phase 3.6 testをすべてskipしてcommand consoleへ直接入ります。
次のbannerが出たらSW1を離して構いません。この経路ではRTC providerだけを初期化し、
SDのcontext作成、card detect開始、初期化、read/writeは行いません。storageを使うのは
`bench-*`や`test-fatfs-time`などを明示的に実行した場合だけです。

```text
[mtfs] boot override: SW1 held; automatic Phase 3.6 test skipped
[mtfs] command console ready (SW1 boot override)
```

SW1はP009へ接続されたactive-low入力です。SW2はこの機能では使用しません。
RTC初期化に失敗した場合、またはcommand consoleを無効にしたbuildでは、自動testへ
fall throughせずcoordinatorを停止します。

### RTC設定とFatFs timestamp確認

通常テスト終了後、RTC設定、FatFs timestamp検証、storage benchmarkを受け付けるcommand consoleが同じDebug Virtual Console上で起動します。`apps/rtc-set`のstandalone RTC consoleとは別のrunner用consoleです。

```text
status
set 2026-08-14 22:30:00
get
test-fatfs-time
```

`set`はtimezone/DST変換を行わないlocal time設定です。`test-fatfs-time`はSD contextとcard detectをその場で再初期化し、一時ファイルへ書き込んだFatFs timestampがRTC時刻（FATの2秒分解能内）と一致することを確認して後片付けします。成功条件は`fatfs_timestamp: PASS`と`FAT timestamp command PASS`です。

software reset後は`status: VALID`のまま時刻が進むことを確認してください。EK-RA8P1のVBATT用J36は未実装なので、ボード電源を完全に切った場合の保持にはJ36へ適切な外部電池を接続する必要があります。詳細は[EK-RA8P1 v1 User's Manual](https://www.renesas.com/en/document/mat/ek-ra8p1-v1-users-manual)と[RA8P1 Group User's Manual: Hardware](https://www.renesas.com/en/document/mah/ra8p1-group-users-manual-hardware)を参照してください。

### 挿抜smoke手順

1. カード未挿入で起動し、`initial ABSENT status PASS`を確認します。
2. `ACTION REQUIRED: INSERT`でカードを挿入します。
3. roundtripと並行testがPASSするまで操作しません。
4. `ACTION REQUIRED: REMOVE`で、I/O停止中にカードを抜きます。
5. `removal contract PASS`と`ACTION REQUIRED: REINSERT`を確認して再挿入します。
6. `fatfs_roundtrip_after_reinsert`、`fatfs_lfn_after_reinsert`、`PHASE 3.6 RUN PASS`を確認します。

各操作待ちは120秒です。共通media層は自動mount/unmountしません。runnerがevent callback後にinitialize/register/mount、またはunmount/unregisterを実行します。

## 期待ログ

容量値やカード種別は媒体により変わります。`55 aa` は情報表示だけでPASS条件ではありません。

```text
[mtfs] RTC provider state=<0:VALID or 1:UNSET> source=SUBCLK local-time vbt=0x.. cold=<0 or 1> source-init=<0 or 1>
[mtfs] EK-RA8P1 Phase 3.6: profile=smoke rounds=1 path=SCI_B SPI+IRQ CD hotplug=on LFN=2 max=64 codepage=437
[mtfs] cache: I=enabled D=enabled fallback=off VTOR=0x22......
[mtfs] vector: [0x22......,0x22......) size=448 line=32 cleans=2
[mtfs] round 1/1 BEGIN
[mtfs] initial ABSENT status PASS: ...
[mtfs] ACTION REQUIRED: INSERT card now; waiting up to 120000 ms
[mtfs] media INSERTED ...
[mtfs] geometry sectors=... size=512 erase=1 type=SDHC/SDXC
[TEST] fatfs_roundtrip: BEGIN
[TEST] fatfs_roundtrip: PASS (...)
[TEST] fatfs_lfn: BEGIN
[TEST] fatfs_lfn: PASS (...)
[TEST] fatfs_concurrent_microtkernel: BEGIN
[TEST] fatfs_concurrent_microtkernel: PASS (...)
[mtfs] ACTION REQUIRED: REMOVE card now; ...
[mtfs] media REMOVED ...
[mtfs] HOTPLUG: removal contract PASS
[mtfs] ACTION REQUIRED: REINSERT card now; waiting up to 120000 ms
[TEST] fatfs_roundtrip_after_reinsert: PASS (...)
[TEST] fatfs_lfn_after_reinsert: PASS (...)
[mtfs] round 1/1 PASS
[mtfs] PHASE 3.6 RUN PASS
[mtfs] command console ready after test run
microT-FS EK-RA8P1 command console
Commands: RTC, FatFs timestamp test, and storage benchmark.
```

失敗時はテスト名、source line、check内容に加え、SD初期化では最後のmtfs/microT-Kernel/FSP errorとR1 responseを表示します。`FR_NO_FILESYSTEM`相当のmount失敗ならカード形式を確認してください。

## fault/lockupのデバッガ確認

Phase 2.1のtest-only fault handlerは、HardFault、MemManage、BusFault、UsageFaultで
`g_mtfs_ra8p1_fault_snapshot`へ情報を保存してBKPT停止します。Expressions viewで
同変数を開き、`valid == 0x4D544653`なら`exception_number`, `cfsr`, `hfsr`,
`mmfar`, `bfar`, `shcsr`, `vtor`, `msp`, `psp`, `exc_return`, `stacked_lr`,
`stacked_pc`を記録してください。

handlerへ入れずlockupした場合はCPUをhaltし、Registers viewで`SCB->CFSR`
(`0xE000ED28`), HFSR (`0xE000ED2C`), MMFAR (`0xE000ED34`), BFAR
(`0xE000ED38`), SHCSR (`0xE000ED24`), VTOR (`0xE000ED08`), IPSR, MSP, PSP,
LRを確認します。LR/EXC_RETURN bit 2が0ならMSP、1ならPSPがfault frameです。
bit 4が1ならstacked PCは`SP + 0x18`、0ならextended FP frameの後
`SP + 0x60`です。`knl_start_mtkernel`, `__wrap_knl_init_interrupt`,
`mtfs_ra8p1_fault_entry`へbreakpointを置くとVTOR変更前後を追跡できます。

## 現時点の制限

- consoleの`diag`でcommon/media/RA SD SPI typed snapshotを取得し、`diag-reset`でcounterだけをresetできます。`test-diagnostics-reset`は試験用contextを初期化し、同一active contextでreset前後のraw sector 0 read、epoch、全counter、保持field、cleanupを自動検証します。applicationはcontext内部の診断fieldを直接参照しません。
- write protect入力は未接続です。
- P409 Card Detectは実機でactive-lowとIRQ6到達を確認済みです。P000 IRQ6-DSは同じ内部IRQ6との競合防止のため無効化します。
- cache coherencyはtarget linker wrapによる互換策です。BSP2側へ同等修正が入ったらADR記載の範囲を削除します。
- FatFs/FSPの呼出し深さとCortex-M85のstack limitを考慮し、並行テストの各workerは16 KiBのstatic user stackを使用します。
- workerは完了通知後にsleepし、coordinatorが結果確認後にterminate/deleteします。共有event flagの削除とtask終了を競合させません。
- read/writeはCMD17/CMD24をsectorごとに反復します。CMD18/CMD25、ACMD23は性能改善候補です。
- CRC7はcommandへ付与しますが、data CRC16は検証しません。
- trim/eraseは未対応です。CSDのerase granularityを未解釈なので、geometryのerase block sizeは暫定1 sectorです。
- SDXCでもexFATは無効です。FATで使用してください。
- RTC validity markerはRA8P1の`VBTBKR[116..127]`を予約します。現在はflat buildです。TrustZone分割時は`VBTBER`と末尾32-byte blockのsecurity/privilege属性をprovider側へ割り当ててください。
- `VBTBER`、`VBTBKR`、`VBTBPSR`の書込みはFSPの`BSP_REG_PROTECT_OM_LPC_BATT` APIで`PRCR.PRC1`保護を一時解除して行います。
- J36は未実装のため、外部VBATTなしの完全な電源断ではRTC/validity marker保持を期待できません。
- 2026-08-12にcache有効、全面無効化fallback offで実機normal 10周を全周完走し、`PHASE 2.1 PASS`を確認しました。Host側もCTest 1/1と並行テスト62 checksがPASSしています。
- 2026-08-14にPhase 3.2 RA Debug build（FSP 6.5.0、Arm GCC 13.2.1）が警告なしで成功しました。実機smokeで未挿入起動、P409/IRQ6挿入、FatFs/並行access、idle抜去後NO_MEDIA、再挿入後の明示initialize/roundtrip、cleanupがPASSしました。
- 2026-08-16にPhase 3.5 RA ReleaseでFatFs roundtrip、`bench-smoke`、`bench-normal`、RTC/FatFs timestamp、idle removal/reinsert、再挿入後roundtripがPASSしました。raw 4 KiB readは273.7 KiB/sでPhase 3.4 baseline 273.6 KiB/sと同等、commonとRA typedのread/write sector数は一致し、SPI error、token/ready timeout、monotonic clock errorは0でした。`diag-reset`後はcounterが0、epochが1となり、initialized/media/geometry/generation/initialization stage/error/bitrateは維持されました。
- 2026-08-16に同一active context用の`test-diagnostics-reset`を実機実行し、40 checks、0 failuresでPASSしました。reset後はcommon/media/RA epochが1、status/geometry/media generation/RA stateが維持され、raw read後にcommon requested/completed sectorとRA read sectorが1へ再増加しました。SPI transfer starts/completionsは124/124、errorとtimeoutは0でした。
- 2026-08-17にPhase 3.6 RA ReleaseのLFN有効／無効buildと静的stack使用量を確認しました。LFN有効の実機normal 10周、`bench-smoke`、`bench-normal`に加え、hotplug有効のnormal 10周で挿入、idle抜去、NO_MEDIA contract、再挿入、再初期化、roundtrip／LFN再試験までPASSしました。SPI error、media error、token／ready timeout、monotonic clock errorは0でした。
- 2026-08-17に既定code pageをCP437へ変更し、Release clean buildがwarning/errorなしで成功しました。実機normal 10周で起動bannerの`codepage=437`、8.3 roundtrip、LFN 45 checks、2-task concurrent、diagnosticsを全周PASSし、SPI errorとtimeoutは0でした。
- stress 100周は未実施です。今回はPhase 3.2の完了判定に含めません。
