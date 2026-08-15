# RA FSP SD SPI Block Device

`mtfs_ra_sd_spi.c` は EK-RA8P1 の SCI_B SPI と microT-Kernel 3.0 の公開デバイスAPIを使い、`mtfs_block_device_t` を提供します。FatFsの型や `diskio.h` には依存しません。状態、FSP設定のRAMコピー、microT-Kernelデバイスディスクリプタ、カード種別、容量、最後のエラーは呼出し側がBSS等に置く `mtfs_ra_sd_spi_context_t` に保持し、ヒープは使いません。

## 配線（EK-RA8P1 PMOD2）

| SD SPI信号 | PMOD2信号 | RA8P1ピン | 設定 |
|---|---|---|---|
| SCK | PMOD2_RTS_SSL | P601 | SCI0 SCK0 |
| MISO/DO | PMOD2_RX | P602 | SCI0 RXD0 |
| MOSI/DI | PMOD2_TX | P603 | SCI0 TXD0 |
| CS | PMOD2_CTS | P604 | GPIO出力、初期High |
| CD | PMOD2_GPIO1 / J25 Pin 9 | P409 | ICU IRQ6、両edge入力 |
| VCC | 3V3 | - | 3.3 Vのみ |
| GND | GND | - | 共通GND |

参照実績と同じくCSにはPMOD2_CTSを使います。SDモジュールは3.3 V SPI対応品を使用し、5 V専用品や信号を5 Vへプルアップするモジュールは接続しないでください。Digilent Pmod MicroSD Revision AのJ1 Pin 9（CD）をPMOD2 J25 Pin 9へ接続します。実機で未挿入`raw=1`、挿入`raw=0`を確認済みのためactive-lowです。write-protect端子は未接続です。

## FSP/BSP2条件

- RA FSP 6.5.0、EK-RA8P1 / R7KA8P1KFLCAC、CPU0
- SCI_B SPI channel 0、master、mode 0（CPOL Low / odd edge）、MSB first
- 初期bitrate 400,000 bps（生成値は約398,089 bps）
- RXI/TXI/TEI/ERI priority 12
- P409はIRQ mode（IRQ6、input pull-up）、External IRQ channel 6（`r_icu`）、両edge、priority 12
- 同じ内部IRQ6へ接続されるP000（IRQ6-DS）のISELは無効化し、IRQ6 inputをP409だけにする
- `p_transfer_tx` / `p_transfer_rx` はNULL。DMA/DTCは使用しません。DTC supportのFSPコンポーネント設定は有効でも、転送インスタンスを接続しません
- callbackは `mtfs_ra_sd_spi_callback`
- 公開ヘッダ `<tk/tkernel.h>`, `r_ioport_api.h`, `r_sci_b_spi.h` のみを利用し、`mtk3_bsp2`内部は変更しません
- microT-Kernelデバイス名は参照実績に合わせて `hspia`

ポート自身が `tk_def_dev()` で `hspia` を登録し、`tk_opn_dev()` で得たdescriptorをcontextに保持します。これはcommit `1ab52cc5a9f59450e62ab78e76de11f4dd89eb15` にSCI_B SPIデバイスドライバを追加せずに統合するためです。SPI転送はFSP公開APIで同期化し、完了通知にはmicroT-Kernel event flagを使います。

## Card Detect契約

`card_present` callbackは任意です。NULLなら従来互換として常時present扱いになり、Card Detect用の追加コストは発生しません。callback指定時はinitialize/status/read/write/sync/geometryと複数sectorのsector間で媒体状態を確認します。

抜去ISRから`mtfs_ra_sd_spi_media_changed_isr()`を呼ぶと、初期化状態を軽量に無効化し、SPI転送待ちevent flagへREMOVED bitを設定します。FSP close、SD command、mutex、FatFs操作はISRでは行いません。起床した通常I/O文脈がSPI channelをcloseし、`MTFS_ERROR_NO_MEDIA`へ収束します。再挿入だけではinitializedへ戻らず、明示的なinitializeでSPIをopenし直してcard typeとgeometryを再取得します。

target側のP409 IRQ callbackはraw levelの記録とmedia/transfer event flag通知だけを行います。edge後のsoftware debounce、INSERTED/REMOVED確定、application callbackはoptional microT-Kernel media serviceのtask文脈です。mount/unmountとregistry操作はアプリケーション方針です。

## プロトコルと制限

初期化はCS Highで80 dummy clocks、CMD0、CMD8、CMD55/ACMD41（v2ではHCS）、CMD58、CMD9の順です。SDSCはbyte addressing、SDHC/SDXCはblock addressingへ変換します。読み書きは512 byte固定で、複数sector要求をCMD17/CMD24の反復として処理します。書込みbusy解除まで待つため `sync` は成功を返します。

### wait / timeout（Phase 3.4a）

data token待ちは1 byteずつdummy `0xFF`を送信し、`0xFE`を成功、`0xFF`を待機継続、それ以外をprotocol errorとして扱います。write ready待ちも1 byteずつpollし、受信が`0xFF`になれば成功、それ以外はbusy継続です。どちらもpollごとの`tk_dly_tsk()`は行いません。DMA、FIFO、multi-byte polling、CMD18/CMD25はこの変更の対象外です。

待機期限はRTCやbenchmark時計ではなく、microT-Kernelの64-bit monotonic operating time (`tk_get_otm`) から求めます。開始時刻とのunsigned差分で判定するため、deadline加算overflowと64-bit counter wrapに依存しません。設定値の単位はmsです。現在のkernel tickは10 msなので期限判定の分解能は10 ms、実際の終了は最大で概ね1 tickと処理中の1 byte転送分だけ遅れる可能性があります。monotonic時計を取得できない場合はkernel errorを通常の`mtfs_error_t`へ変換して待機を中止します。

ACMD41は同じmonotonic deadlineで初期化全体の再試行期限を判定し、各失敗attemptの間に1 msを要求する`tk_dly_tsk()`を残します。この要求は10 ms tickへ切り上げられますが、retry回数をtimeout時間とみなさないため、tick変更でtimeoutが10倍になることはありません。SPI transfer callback用event flagの`transfer_timeout_ms`は独立した転送停止検出として従来どおり維持します。

`mtfs_ra_sd_spi_diagnostics_t`にはtoken/readyのwait回数、総poll byte数、1回あたり最大poll数、timeout回数、ACMD41 attempt数、monotonic clock error数を記録します。Phase 3.4aで追加したcounterは`UINT32_MAX`で飽和し、既存counterは従来どおり32-bit wrapです。公開context構造体へfieldを追加したため、この版へ更新する利用側はportとapplicationを一緒に再buildしてください。

`trim` は未対応です。CSDのERASE_BLK_EN/SECTOR_SIZEをまだ解釈しないため、FatFsへ返すerase block sizeは安全な暫定値1 sectorです。CMD18/CMD25、ACMD23、CSDからのerase granularity取得、CRC16検証、write protectは今後の改善項目です。物理抜去後の未保存dataやopen中のFILは救済・再開しません。
