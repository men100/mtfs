# Applications

microT-FS の利用例とコンテスト向け統合デモを配置します。本番ライブラリはここへ置かず、`src/` を利用する側の構成として保ちます。

- `rtc-set/`: `set/get/status/clear/help`を持つUART非依存parserと、EK-RA8P1／STM32N6570-DKで単独起動できるtarget firmware
- `key-provision/`: RFP注入済みのRA8P1 HUK-wrapped fleet keyを検証し、SDへ一度だけ保存する専用firmware
