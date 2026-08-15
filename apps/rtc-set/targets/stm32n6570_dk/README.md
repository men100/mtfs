# STM32N6570-DK standalone RTC console

STM32N6570-DKでRTCの設定・確認だけを行うSTM32CubeIDE firmwareです。SDMMC、FatFs、
microT-FS test runnerを起動せず、microT-Kernel開始後すぐにUSART1 VCPのconsoleへ入ります。

## Import / build

対応環境はSTM32CubeIDE 2.1.1、STM32Cube FW_N6 V1.3.0、μT-Kernel BSP2 v1.00.04です。

1. CubeIDEの`File > Import > General > Existing Projects into Workspace`で、次の2プロジェクトをimportします。
   - `boards/stm32n6570_dk/FSBL` (`mtfs_stm32n6570_dk_FSBL`)
   - `apps/rtc-set/targets/stm32n6570_dk/Appli` (`mtfs_stm32n6570_dk_rtc_set`)
2. `Copy projects into workspace`は無効にします。
3. `mtfs_stm32n6570_dk_rtc_set`のDebug、次に共通FSBLのDebugをbuildします。
4. FSBLのDebug Configurationを作り、Startup/Load imagesへ
   `apps/rtc-set/targets/stm32n6570_dk/Appli/Debug/mtfs_stm32n6570_dk_rtc_set.elf`
   をdownload + symbols対象として追加します。Appliを先にSRAMへloadし、FSBLをload/startします。

共通FSBL、HAL/CMSIS、ExtMem資産は`boards/stm32n6570_dk/`に1つだけ置き、このAppliと
SD/FatFs runnerで共有します。

## Console

USART1 VCPを115200 bps、8 data bits、no parity、1 stop bit、flow controlなしで開きます。
起動後は次のコマンドを使用できます。

```text
status
set 2026-08-14 21:30:00
get
clear
help
```

`test-fatfs-time`はありません。このfirmwareはRTC設定専用で、FatFs/SD driver/test caseを
リンクしません。時刻はtimezone/UTC/DST変換を行わないlocal timeです。

RTC sourceはLSI、設定markerはTAMP backup register 28-30です。software system resetでは
時刻とmarkerを保持します。STM32N6570-DKでVDD-off中のRTC進行は保証しません。

起動例:

```text
[mtfs] STM32N6570-DK standalone RTC console
[mtfs] RTC provider state=1 source=LSI local-time reset=0x00000000
microT-FS RTC set (local time; no timezone/DST conversion)
Type help for commands.
> 
```
