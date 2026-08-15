# EK-RA8P1 standalone RTC console

microT-FSのRA8P1 RTC providerと`rtc-set` parserだけを起動するe² studio
プロジェクトです。SD、FatFs、media lifecycle、並行access試験は実行しません。

## 必要環境

- e² studio 2026-04.2以降
- Renesas RA FSP 6.5.0
- GNU Arm Embedded 13.2.1.arm-13-7
- EK-RA8P1（CPU0）
- `mtk3_bsp2` submodule v1.00.04 (`1ab52cc5`)

e² studioの **File > Import > General > Existing Projects into Workspace** で
このdirectoryを指定し、必要なら`configuration.xml`から **Generate Project
Content** を実行してからDebugまたはReleaseをbuildしてください。

FSP構成はI/O PortとSub-Clock動作のRTCだけです。生成されるfirmwareには
microT-FSのBlock Device、SD、FatFs、共通test frameworkをリンクしません。
確認時のサイズはDebugがtext 36,268 bytes / BSS 13,016 bytes、Releaseが
text 24,332 bytes / BSS 13,020 bytesです。

書込み後はSCI8のデバッグ通信を115200 bps、8 data bits、no parity、1 stop bitで
開きます。CoolTermの推奨設定は次のとおりです。

- Terminal Mode: Raw Mode
- Enter Key Emulation: CR（CR+LFも受理）
- Local Echo: OFF
- Handle BS and DEL Characters: ON
- Convert Non-printable Characters: OFF
- Ignore Line Feed Character: OFF
- Handle CR as real Carriage Return: ON

起動後はテストを待たず、すぐRTC consoleへ入ります。

```text
[mtfs] EK-RA8P1 standalone RTC console
microT-FS RTC set (local time; no timezone/DST conversion)
Type help for commands.
> status
status: UNSET
> set 2026-08-15 12:34:56
OK: RTC set and marker committed
> get
time: VALID 2026-08-15 12:34:56
```

利用可能なcommandは`set/get/status/clear/help`です。この最小構成にはSD/FatFsを
必要とする`test-fatfs-time`を含めません。

固定しているBSP2 v1.00.04のRA8P1受信処理とRAM vector D-cache問題については、
target内のlinker wrapで互換処理を適用します。BSP2へ上流修正が入った時点で保存patch、
wrapper source、該当`--wrap` optionをまとめて削除してください。
