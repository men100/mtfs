# EK-RA8P1 Phase 3.2 SD SPI / Card Detect runner

FATで事前フォーマットしたDigilent Pmod MicroSD Revision AをPMOD2へ接続し、microT-FSのBlock Device、FatFs round-trip、microT-Kernel 2タスク並行アクセス、P409/IRQ6による挿入・抜去・再挿入を確認するe² studioプロジェクトです。テストはカードをフォーマットしません。

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

SDカードはPC等でFAT12/FAT16/FAT32のいずれかへ事前フォーマットしてください。exFAT、NTFS、未フォーマット媒体には対応しません。テストはルートへ `RTTEST.BIN`, `TASKA.BIN`, `TASKB.BIN` を一時作成して最後に削除するため、同名の既存ファイルがないカードを使ってください。

## FSP設定

同梱 `configuration.xml` は動作実績のpin/clock条件だけを現在のrunnerへ反映しています。

- SCI_B SPI channel 0、master、mode 0、MSB first
- 初期要求400 kHz（実生成約398,089 Hz）、初期化成功後4 MHzへ再設定
- callback: `mtfs_ra_sd_spi_callback`
- RXI/TXI/TEI/ERI priority 12
- TX/RX transfer instanceはNULL（DMA/DTC未使用）
- P601/P602/P603をSCI0 SCK/RXD/TXD、P604を初期HighのGPIO output
- P409をGPIO IRQ input、External IRQ channel 6、両edge、priority 12
- callback: `mtfs_ra8p1_card_detect_callback`

Card Detectは100 msのsoftware debounceを既定とし、edge後だけoptional media service taskが再確認します。`MTFS_RA8P1_CD_DEBOUNCE_MS`で変更できます。Pmod回路からactive-lowを初期候補として`MTFS_RA8P1_CD_ACTIVE_LOW=1`にしていますが、実機raw level未確認のためまだ確定していません。未挿入で`raw=1`、挿入で`raw=0`になることをログまたはデバッガで確認してください。逆なら同defineを0へ変更します。

指定のmtk3_bsp2 v1.00.04はRAM例外ベクタのcopy、kernel例外登録、実行中の
`tk_def_int()`更新後にD-cache cleanを行いません。Phase 2.1ではsubmoduleを変更
せず、targetのlinker wrapで各更新範囲をcleanしてDSB/ISBを実行します。通常build
ではI-cache/D-cacheを無効化しません。codeは更新していないためI-cache invalidate
も行いません。根本原因、処理順、上流patch、削除条件は
`docs/adr/0001-ra8p1-vector-cache-coherency.md`を参照してください。

FatFs設定はcompile definitionと `src/mtfs_config.h` により、read/write有効、`FF_FS_REENTRANT=1`、microT-Kernel mutex adapter、`FF_FS_NORTC=1`、1 volume、`FF_USE_MKFS=0`です。

## ビルド

1. e² studioで **File > Import > General > Existing Projects into Workspace** を選び、この `basic` directoryを指定します。
2. `configuration.xml` を開き、FSP 6.5.0 packが選択されていることを確認します。
3. 必要なら **Generate Project Content** を実行します。
4. configurationを **Debug** にして **Project > Build Project** を実行します。

成功時は `Debug/mtfs_ek_ra8p1_basic.elf` と `.srec` が生成されます。Phase 3.2
Debug buildの確認値はtext約75 KiB、BSS約54 KiBです。coordinatorと並行test workerに
加え、明示的に組み込んだmedia serviceの2 KiB static stackを含みます。

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

実機試験はSDへ書込みを行います。重要データのないカードで実施してください。runnerが`ACTION REQUIRED`を表示するまでは抜き差しせず、active write中の抜去は行わないでください。

### 挿抜smoke手順

1. カード未挿入で起動し、`initial ABSENT status PASS`を確認します。
2. `ACTION REQUIRED: INSERT`でカードを挿入します。
3. roundtripと並行testがPASSするまで操作しません。
4. `ACTION REQUIRED: REMOVE`で、I/O停止中にカードを抜きます。
5. `removal contract PASS`と`ACTION REQUIRED: REINSERT`を確認して再挿入します。
6. `fatfs_roundtrip_after_reinsert`と`PHASE 3.2 RUN PASS`を確認します。

各操作待ちは120秒です。共通media層は自動mount/unmountしません。runnerがevent callback後にinitialize/register/mount、またはunmount/unregisterを実行します。

## 期待ログ

容量値やカード種別は媒体により変わります。`55 aa` は情報表示だけでPASS条件ではありません。

```text
[mtfs] EK-RA8P1 Phase 3.2: profile=smoke rounds=1 path=SCI_B SPI+IRQ CD hotplug=on
[mtfs] cache: I=enabled D=enabled fallback=off VTOR=0x22......
[mtfs] vector: [0x22......,0x22......) size=448 line=32 cleans=2
[mtfs] round 1/10 BEGIN
[mtfs] initial ABSENT status PASS: ...
[mtfs] ACTION REQUIRED: INSERT card now; waiting up to 120000 ms
[mtfs] media INSERTED ...
[mtfs] geometry sectors=... size=512 erase=1 type=SDHC/SDXC
[TEST] fatfs_roundtrip: BEGIN
[TEST] fatfs_roundtrip: PASS (...)
[TEST] fatfs_concurrent_microtkernel: BEGIN
[TEST] fatfs_concurrent_microtkernel: PASS (...)
[mtfs] ACTION REQUIRED: REMOVE card now; ...
[mtfs] media REMOVED ...
[mtfs] HOTPLUG: removal contract PASS
[mtfs] ACTION REQUIRED: REINSERT card now; waiting up to 120000 ms
[TEST] fatfs_roundtrip_after_reinsert: PASS (...)
[mtfs] round 1/1 PASS
[mtfs] PHASE 3.2 RUN PASS
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

- write protect入力は未接続です。
- P409 Card Detectのactive levelとIRQ実機到達は未確認です。回路情報だけでactive-low確定とは記載しません。
- cache coherencyはtarget linker wrapによる互換策です。BSP2側へ同等修正が入ったらADR記載の範囲を削除します。
- FatFs/FSPの呼出し深さとCortex-M85のstack limitを考慮し、並行テストの各workerは16 KiBのstatic user stackを使用します。
- workerは完了通知後にsleepし、coordinatorが結果確認後にterminate/deleteします。共有event flagの削除とtask終了を競合させません。
- read/writeはCMD17/CMD24をsectorごとに反復します。CMD18/CMD25、ACMD23は性能改善候補です。
- CRC7はcommandへ付与しますが、data CRC16は検証しません。
- trim/eraseは未対応です。CSDのerase granularityを未解釈なので、geometryのerase block sizeは暫定1 sectorです。
- SDXCでもexFATは無効です。FATで使用してください。
- 2026-08-12にcache有効、全面無効化fallback offで実機normal 10周を全周完走し、`PHASE 2.1 PASS`を確認しました。Host側もCTest 1/1と並行テスト62 checksがPASSしています。
- 2026-08-14にPhase 3.2 RA Debug build（FSP 6.5.0、Arm GCC 13.2.1）が警告なしで成功しました。P409/IRQ6の実機挿抜試験は未実施です。
- stress 100周は未実施です。今回はPhase 3.2の完了判定に含めません。
