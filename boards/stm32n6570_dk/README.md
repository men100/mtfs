# STM32N6570-DK shared Cube support

STM32N6570-DK向けアプリケーションで共用するSTM32Cube生成資産です。

- `FSBL/`: 共通First Stage Boot Loaderプロジェクト
- `Drivers/`: STM32Cube FW_N6 HAL/CMSIS
- `Middlewares/`: FSBLが使用するSTM32 ExtMem Manager
- `Secure_nsclib/`: Secure application用interface library

FSBLはアプリケーションごとに複製しません。現在、次のCubeIDE Appliがこのディレクトリを
linked resource/include pathで参照します。

- `tests/targets/stm32n6570_dk/basic/Appli`: SDMMC/FatFs実機runner
- `apps/rtc-set/targets/stm32n6570_dk/Appli`: RTC設定専用firmware

使用ツールはSTM32CubeIDE 2.1.1、STM32CubeMX 6.17系、STM32Cube FW_N6 V1.3.0です。
CubeIDEへは`FSBL/`を既存プロジェクトとして直接importし、`Copy projects into workspace`を
無効にします。プロジェクト名は`mtfs_stm32n6570_dk_FSBL`です。

`tests/targets/stm32n6570_dk/basic/mtfs_stm32n6570_dk.ioc`を再生成すると、test target側へ
`Drivers`、`Middlewares`、`Secure_nsclib`、`FSBL`が再作成される場合があります。その場合は
生成差分をこの共通ディレクトリへ反映し、test target側に複製を残さないでください。
