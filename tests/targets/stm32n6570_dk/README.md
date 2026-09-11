# STM32N6570-DK target test project

このdirectoryのmulti-projectをSTM32CubeIDEへimportし、
`mtfs_stm32n6570_dk_test_FSBL`と`mtfs_stm32n6570_dk_test_Appli`をそれぞれ
DebugまたはReleaseでclean buildします。通常のstorage testはAppliのIDMA設定を
使用します。`Appli/Debug`、`Appli/Release`、`FSBL/Debug`、`FSBL/Release`は
build生成物であり追跡対象ではありません。

## SD pin configuration

on-board microSD socketはboard回路上でSDMMC2へ固定接続されています。

| Signal | MCU pin | Configuration boundary |
|---|---|---|
| D0 | PC4 | Cube `.ioc` / generated MSP |
| D1 | PC5 | Cube `.ioc` / generated MSP |
| D2 | PC0 | Cube `.ioc` / generated MSP |
| D3 | PE4 | Cube `.ioc` / generated MSP |
| CK | PC2 | Cube `.ioc` / generated MSP |
| CMD | PC3 | Cube `.ioc` / generated MSP |
| Card Detect | PN12 / EXTI12 | Cube `.ioc` plus board port IRQ registration |
| Write Protect | not connected | socketからsoftware入力への固定接続なし |

SDMMC2 alternate-function pin、PN12 pull-up/edge設定、SDMMC2 IRQ priority 5、
EXTI12 IRQ priority 6は `mtfs_stm32n6570_dk_test.ioc` がsource of truthです。
CubeMX/CubeIDE Pinout & Configurationで変更してcode generationを実行し、
generated `main.h`、`main.c`、`stm32n6xx_hal_msp.c`を直接編集しないでください。
board portが直接参照するCard DetectのC alias、active-low、IRQは
`src/ports/stm32_cube/boards/stm32n6570_dk/mtfs_stm32n6570_dk_board_config.h`
に集約しています。このheaderだけではSDMMC2 pinmuxやboard固定接続は変更できません。

Card Detectはactive-lowです。SDMMC2 data transfer IRQとCard Detect EXTI12は別の
IRQ channelであり、いずれもCube設定とboard portの登録を一致させる必要があります。

## Console

`help`、`help <group>`、`help all`でcommandを確認できます。
`log-level`は現在値を表示し、`log-level off|error|info|debug`でresetまでの実行時
レベルを変更します。Debug/Release build configurationとは独立しています。
