# EK-RA8P1 target test project

このdirectoryをe² studioへ既存projectとしてimportし、`mtfs_ek_ra8p1`の
DebugまたはRelease configurationをclean buildします。FSP生成物（`ra/`、
`ra_cfg/`、`ra_gen/`）とbuild directoryは追跡対象ではありません。

## SD/Pmod pin configuration

現行のDigilent Pmod MicroSD接続は次のとおりです。

| Signal | EK-RA8P1 signal / MCU pin | Configuration boundary |
|---|---|---|
| SCK | PMOD2_RTS_SSL / P601 | FSP `configuration.xml` (SCI0 SCK0) |
| MISO | PMOD2_RX / P602 | FSP `configuration.xml` (SCI0 RXD0) |
| MOSI | PMOD2_TX / P603 | FSP `configuration.xml` (SCI0 TXD0) |
| CS | PMOD2_CTS / P604 | `mtfs_ra8p1_board_config.h`; software GPIO output |
| Card Detect | PMOD2_GPIO1 / P409 | `mtfs_ra8p1_board_config.h` plus FSP external IRQ6 |
| Write Protect | not connected/used | no software write-protect signal |

SCK/MISO/MOSIのalternate function、P409のpinmux、外部IRQ channel 6とcallbackは
このprojectの `configuration.xml` がsource of truthです。変更時はFSP Pins/Stacksで
編集してcode generationを実行し、生成fileを直接編集しないでください。P409をIRQ6へ
割り当てる一方、P000は通常GPIO inputかつIRQ無効のままにしてIRQ6競合を防ぎます。
CSとCard DetectのC側alias、active-low、debounce値は
`src/ports/ra_fsp/boards/ek_ra8p1/mtfs_ra8p1_board_config.h`にあります。
これらのmacroだけではFSP pinmuxやboard/Pmod間の物理配線は変更できません。

Card Detectはactive-lowです。IRQ callbackはedgeを通知するだけで、debounceとmedia
state更新はtask contextで行います。

## Console

`help`、`help <group>`、`help all`でcommandを確認できます。
`log-level`は現在値を表示し、`log-level off|error|info|debug`でresetまでの実行時
レベルを変更します。Debug/Release build configurationとは独立しています。
