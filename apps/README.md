# Applications

microT-FS の利用例とコンテスト向け統合デモを配置します。本番ライブラリはここへ置かず、`src/` を利用する側の構成として保ちます。

- `rtc-set/`: `help/rtc-get/rtc-status/rtc-set/rtc-clear`を持つ、診断console共有のUART非依存parser
- `key-provision/`: UARTで受信したRA8P1 fleet keyをHUK-wrapし、board上OSPIへ保存する専用firmware
