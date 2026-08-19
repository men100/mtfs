# Documentation

アーキテクチャ、API、移植、セキュリティモデル、テスト方針など、ルート README より詳細な文書を配置します。

- `adr/0001-ra8p1-vector-cache-coherency.md`: EK-RA8P1 Phase 2.1のRAM vector cache coherency判断
- `adr/0002-ra8p1-tmonitor-receive.md`: EK-RA8P1 T-Monitor受信待ち不具合のtarget互換修正
- `adr/0003-secure-ai-storage.md`: Phase 4.0 Secure AI Storageの脅威モデル、鍵境界、format判断
- `security/secure-ai-storage.md`: sealed model形式、crypto provider/API、extension構成、実装検証計画
- `security/host-sealed-model-tools.md`: Phase 4.1A Host toolのbuild、運用、test vector、制限
- `security/ek-ra8p1-rsip-e50d-spike.md`: Phase 4.1B-RA RSIP-E50D hardware spikeのpreflight、blocker、test-only harness
- `security/ek-ra8p1-sd-key-provisioning.md`: RFP注入したHUK-wrapped fleet keyを専用appでSDへ保存するcontest profile
- `porting/README.md`: 新規ボードへのソース取り込みから実機testまでの移植ガイド
- `porting/block-device-port.md`: Block Device operation、状態、error、contextの実装契約
- `porting/microtkernel-integration.md`: mutex、IRQ、event flag、HAL timebase、task寿命
- `porting/dma-cache-coherency.md`: DMA buffer、cache、barrier、RIFの確認項目
- `porting/testing-a-new-port.md`: 新規target runnerの構成、試験順序、checklist
- `testing/performance-benchmark.md`: Phase 3.4非破壊benchmarkの条件、指標、再現手順、baseline表
- `diagnostics.md`: Phase 3.5 structured diagnostics snapshot、reset、整合性、AI telemetry契約
