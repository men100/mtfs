# EK-RA8P1 Phase 2 basic runner

FATで事前フォーマットしたSDカードをPMOD2へSPI接続し、microT-FSのBlock Device、FatFs round-trip、microT-Kernel 2タスク並行アクセスを順に確認するe² studioプロジェクトです。テストはカードをフォーマットしません。

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

指定のmtk3_bsp2 v1.00.04では、RA8P1のRAM例外ベクタ更新後に必要な
cache clean/invalidate処理がまだ入っていません。古いベクタを参照すると
Cortex-M85が`0xEFFFFFFE`のlockup状態へ入るため、このターゲットは
`hal_entry()`でI/D cacheを無効化してからmicroT-Kernelを起動します。
BSP2をベクタテーブルのcache maintenance対応版へ更新した後は、この互換策を
削除してcacheを有効化できます。

FatFs設定はcompile definitionと `src/mtfs_config.h` により、read/write有効、`FF_FS_REENTRANT=1`、microT-Kernel mutex adapter、`FF_FS_NORTC=1`、1 volume、`FF_USE_MKFS=0`です。

## ビルド

1. e² studioで **File > Import > General > Existing Projects into Workspace** を選び、この `basic` directoryを指定します。
2. `configuration.xml` を開き、FSP 6.5.0 packが選択されていることを確認します。
3. 必要なら **Generate Project Content** を実行します。
4. configurationを **Debug** にして **Project > Build Project** を実行します。

成功時は `Debug/mtfs_ek_ra8p1_basic.elf` と `.srec` が生成されます。確認済み構成のサイズ目安はtext約65 KiB、BSS約18 KiBです。

## 書込みと実行

1. ボードのPMOD2へ電源OFF状態で配線し、FATカードを挿入します。
2. e² studioでEK-RA8P1 / J-LinkのRenesas GDB Hardware Debug構成を作り、`Debug/mtfs_ek_ra8p1_basic.elf` を指定します。
3. download後にCPUをresumeします。
4. e² studio Debug Virtual Console（`tm_printf`出力）を確認します。

実機試験はSDへ書込みを行います。重要データのないカードで実施し、実行中は抜かないでください。

## 期待ログ

容量値やカード種別は媒体により変わります。`55 aa` は情報表示だけでPASS条件ではありません。

```text
[mtfs] EK-RA8P1 Phase 2 test start
[mtfs] SD init PASS: SDHC/SDXC, SPI=4000000 Hz
[mtfs] geometry PASS: sectors=... sector_size=512 erase=1
[mtfs] sector 0 read PASS; signature=55 aa (present)
[TEST] fatfs_roundtrip: BEGIN
[TEST] fatfs_roundtrip: PASS (...)
[TEST] fatfs_concurrent_microtkernel: BEGIN
[TEST] fatfs_concurrent_microtkernel: PASS (...)
[mtfs] PHASE 2 PASS
```

失敗時はテスト名、source line、check内容に加え、SD初期化では最後のmtfs/microT-Kernel/FSP errorとR1 responseを表示します。`FR_NO_FILESYSTEM`相当のmount失敗ならカード形式を確認してください。

## 現時点の制限

- card detectとwrite protect入力は未接続で、通電中のhot plugを扱いません。
- 指定BSP2版のRAM例外ベクタcache coherency対策として、現在はI/D cacheを無効化しています。
- FatFs/FSPの呼出し深さとCortex-M85のstack limitを考慮し、並行テストの各workerは16 KiBのstatic user stackを使用します。
- workerは完了通知後にsleepし、coordinatorが結果確認後にterminate/deleteします。共有event flagの削除とtask終了を競合させません。
- read/writeはCMD17/CMD24をsectorごとに反復します。CMD18/CMD25、ACMD23は性能改善候補です。
- CRC7はcommandへ付与しますが、data CRC16は検証しません。
- trim/eraseは未対応です。CSDのerase granularityを未解釈なので、geometryのerase block sizeは暫定1 sectorです。
- SDXCでもexFATは無効です。FATで使用してください。
- 実機へのdownloadとSD媒体試験は、接続されたボード上で別途実施する必要があります。
