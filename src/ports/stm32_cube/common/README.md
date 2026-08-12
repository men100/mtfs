# STM32Cube common

`mtfs_stm32_hal_timebase` は、microT-Kernel 起動後に HAL の `uwTick` を進めます。STM32Cube の既定 SysTick handler は T-Kernel の tick と競合するため使用せず、`CNF_TIMER_PERIOD` と同周期の T-Kernel cyclic handler から `HAL_IncTick()` を呼びます。周期 1/10/100 ms に対応し、最初の acquire で生成・開始、最後の release で停止・削除する参照カウント方式です。

呼び出し条件:

- `mtfs_stm32_hal_timebase_acquire()` / `release()` はカーネル起動後の task context から呼ぶ。
- SDMMC context で `manage_hal_timebase=1` とすれば initialize/deinitialize に連動する。
- HAL の timeout を使う別ドライバも同じ timebase を共有できる。
- `HAL_SetTickFreq()` は SysTick を再構成するため呼ばず、実周波数に対応する `uwTickFreq` を設定する。
