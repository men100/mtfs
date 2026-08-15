# STM32N6570-DK test runners

STM32N6570-DK固有のSDMMC2、card-detect、RIF、CubeIDE Appli設定は各runnerのtarget層に置きます。共通FSBLとCube生成資産は`boards/stm32n6570_dk/`に置き、RTC設定アプリを含む複数Appliで共有します。共有Block Device/FatFs/test caseにボード依存を追加しません。

現在は `basic/` が SDMMC2 4-bit、D-cache 有効、IDMA+IRQ 既定、polling fallback を検証します。
