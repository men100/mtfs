# STM32N6570-DK test runners

STM32N6570-DK 固有の SDMMC2、card-detect、RIF、CubeIDE FSBL/Appli 設定は各 runner の target 層に置きます。共有 Block Device/FatFs/test case にボード依存を追加しません。

現在は `basic/` が SDMMC2 4-bit、D-cache 有効、IDMA+IRQ 既定、polling fallback を検証します。
