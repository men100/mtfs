# DMAとcache coherency

この章はDMAを使うBlock Device portの独立チェックリストです。cache maintenanceの
API名、cache line size、DMA可能RAM、security属性はMCUごとに確認します。

## bufferの所有範囲

cache maintenanceは指定addressを含むcache line全体へ作用します。未整列のuser bufferを
外向きに丸めてinvalidateすると、同じlineにあるCPUのdirty dataを失う可能性があります。
そのためSTM32 SDMMC portはuser bufferへ直接DMAせず、context内のaligned bounce buffer
だけをDMA対象にします。bufferの開始、長さ、sectionの全てがcache line単位で完結し、
隣接objectとlineを共有しないことをlinker mapでも確認します。

一般的な所有権の遷移は次です。

### DMA read

1. DMA前にbounce範囲をclean/invalidateし、CPUのdirty lineをRAMへ反映して古いcacheを捨てる。
2. barrier後にDMAを開始する。
3. IRQ/callback完了後にbounce範囲をinvalidateする。
4. barrier後にbounce bufferからuser bufferへcopyする。

### DMA write

1. user bufferからbounce bufferへcopyする。
2. bounce範囲をD-cache cleanする。
3. barrier後にDMAを開始する。
4. 完了とcard transfer stateを確認してreturnする。

CPU/DMA、cache API、bus fabricの仕様により追加barrierが必要な場合があります。CMSIS関数を
呼んだ事実だけでcoherencyを仮定せず、対象MCUのreference manualとmap/実機を照合します。

## STM32N6570-DK参照値

現在のSTM32 port/runnerでは次を使用します。これはSTM32N6570-DKの参照構成であり、
他MCUの固定値ではありません。

- bounce size: 4096 byte（512 byte x 8 sector）
- alignment/cache line: 32 byte
- linker RAM: `0x34080000`から1536 KiB
- transfer: SDMMC2 IDMA + IRQ
- RIF: SDMMC2 RIMC master index 3をCID1/Secure/Privilegedに設定
- SDMMC2 slave: Secure/Privilegedとしてreadback確認

`idma_platform_ready` hookはtargetのRIF等が期待値であることをinitialize前に検査します。
DMAを開始するperipheralがCPUと異なるbus master属性を持つ場合、MPUのCPU accessだけでなく、
DMA masterのsecurity、privilege、CID、firewall設定も必要です。

## IRQ、timeout、復旧

DMA開始だけでなく、SDMMC IRQ entry、HAL Rx/Tx callback、event flag通知が到達することを
counterやbreakpointで確認します。timeout時は転送active状態を解除し、HAL abort、event
bit clear、未初期化化を行います。次のinitializeがHALを既知状態へ戻せること、timeout後の
遅延IRQを次のrequest完了と誤認しないことを確認します。

polling fallbackはDMA/RIF/IRQ問題の切り分けに使えますが、polling成功だけでIDMA経路を
合格にしてはいけません。両経路は同じBlock Device APIを使う一方、内部転送と前提条件が
異なります。

## RA8P1 RAM vectorとの区別

EK-RA8P1のADR 0001は、microT-KernelのRAM vector tableをCPU exception fetchへ可視化する
ためのD-cache clean/barrierです。RA SPI portはDMA/DTCを使いません。したがってRAM vector
cache coherency対策と、STM32 SDMMC IDMA data bufferのcache coherencyは別問題です。一方を
適用しても他方の検証を省略できません。

## コピー用チェックリスト

- [ ] DMA engineから到達できるRAM領域をreference manualで確認した
- [ ] linker scriptとmapでbufferの実address/sectionを確認した
- [ ] 開始addressとsizeがcache lineにalignedである
- [ ] bufferの先頭/末尾cache lineを別objectと共有しない
- [ ] user bufferへ直接DMAする場合のalignmentと所有権を証明した
- [ ] 証明できないuser bufferにはbounce bufferを使う
- [ ] read前後のclean/invalidate順序を確認した
- [ ] write前にD-cache cleanする
- [ ] invalidateで隣接dirty dataを失わない
- [ ] cache maintenanceとDMA開始/完了の間に必要なbarrierを入れた
- [ ] DMA bus masterのsecurity/privilege/CID/RIF設定を確認した
- [ ] IRQ番号、priority、handler登録、HAL callback到達を確認した
- [ ] timeout時にabortし、未初期化状態へ戻す
- [ ] timeout後の再initializeと次の転送を確認した

