# STM32Cube port

STM32 HAL を使用するターゲット向けポートです。ボード固有のハンドル、IRQ、card-detect、RIF 設定はここへ持ち込まず、ターゲットから構成として渡します。

- `common/`: microT-Kernel 起動後の HAL timebase など Cube 共通処理
- `sdmmc/`: HAL SDMMC を `mtfs_block_device_t` に変換する同期ブロックデバイス

現在の実動構成は STM32N6570-DK です。HAL 型名の互換性を満たす別 STM32 ファミリでは、`MTFS_STM32_HAL_HEADER` とターゲット構成を差し替えて再利用できます。
