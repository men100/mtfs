# Documentation

アーキテクチャ、API、移植、セキュリティモデル、テスト方針など、ルート README より詳細な文書を配置します。

- `adr/0001-ra8p1-vector-cache-coherency.md`: EK-RA8P1 Phase 2.1のRAM vector cache coherency判断
- `porting/README.md`: 新規ボードへのソース取り込みから実機testまでの移植ガイド
- `porting/block-device-port.md`: Block Device operation、状態、error、contextの実装契約
- `porting/microtkernel-integration.md`: mutex、IRQ、event flag、HAL timebase、task寿命
- `porting/dma-cache-coherency.md`: DMA buffer、cache、barrier、RIFの確認項目
- `porting/testing-a-new-port.md`: 新規target runnerの構成、試験順序、checklist
