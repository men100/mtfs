# RA FSP SD SPI Block Device

`mtfs_ra_sd_spi.c` は EK-RA8P1 の SCI_B SPI と microT-Kernel 3.0 の公開デバイスAPIを使い、`mtfs_block_device_t` を提供します。FatFsの型や `diskio.h` には依存しません。状態、FSP設定のRAMコピー、microT-Kernelデバイスディスクリプタ、カード種別、容量、最後のエラーは呼出し側がBSS等に置く `mtfs_ra_sd_spi_context_t` に保持し、ヒープは使いません。

## 配線（EK-RA8P1 PMOD2）

| SD SPI信号 | PMOD2信号 | RA8P1ピン | 設定 |
|---|---|---|---|
| SCK | PMOD2_RTS_SSL | P601 | SCI0 SCK0 |
| MISO/DO | PMOD2_RX | P602 | SCI0 RXD0 |
| MOSI/DI | PMOD2_TX | P603 | SCI0 TXD0 |
| CS | PMOD2_CTS | P604 | GPIO出力、初期High |
| VCC | 3V3 | - | 3.3 Vのみ |
| GND | GND | - | 共通GND |

参照実績と同じくCSにはPMOD2_CTSを使います。SDモジュールは3.3 V SPI対応品を使用し、5 V専用品や信号を5 Vへプルアップするモジュールは接続しないでください。MOSI/CSのプルアップを備えた市販モジュールを想定します。カードの抜き差し検出とwrite-protect端子は未接続です。

## FSP/BSP2条件

- RA FSP 6.5.0、EK-RA8P1 / R7KA8P1KFLCAC、CPU0
- SCI_B SPI channel 0、master、mode 0（CPOL Low / odd edge）、MSB first
- 初期bitrate 400,000 bps（生成値は約398,089 bps）
- RXI/TXI/TEI/ERI priority 12
- `p_transfer_tx` / `p_transfer_rx` はNULL。DMA/DTCは使用しません。DTC supportのFSPコンポーネント設定は有効でも、転送インスタンスを接続しません
- callbackは `mtfs_ra_sd_spi_callback`
- 公開ヘッダ `<tk/tkernel.h>`, `r_ioport_api.h`, `r_sci_b_spi.h` のみを利用し、`mtk3_bsp2`内部は変更しません
- microT-Kernelデバイス名は参照実績に合わせて `hspia`

ポート自身が `tk_def_dev()` で `hspia` を登録し、`tk_opn_dev()` で得たdescriptorをcontextに保持します。これはcommit `1ab52cc5a9f59450e62ab78e76de11f4dd89eb15` にSCI_B SPIデバイスドライバを追加せずに統合するためです。SPI転送はFSP公開APIで同期化し、完了通知にはmicroT-Kernel event flagを使います。

## プロトコルと制限

初期化はCS Highで80 dummy clocks、CMD0、CMD8、CMD55/ACMD41（v2ではHCS）、CMD58、CMD9の順です。SDSCはbyte addressing、SDHC/SDXCはblock addressingへ変換します。読み書きは512 byte固定で、複数sector要求をCMD17/CMD24の反復として処理します。書込みbusy解除まで待つため `sync` は成功を返します。

`trim` は未対応です。CSDのERASE_BLK_EN/SECTOR_SIZEをまだ解釈しないため、FatFsへ返すerase block sizeは安全な暫定値1 sectorです。CMD18/CMD25、ACMD23、CSDからのerase granularity取得、CRC16検証、card detect/write protectは今後の改善項目です。
