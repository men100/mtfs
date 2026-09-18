# target profile

共通APIはportableですが、Storage Sentinelのclassification qualityを評価する際のreference profileは、targetごとに固定されています。Debug buildやfallback経路は機能確認や診断には利用できますが、classification qualityを評価する際の基準には含まれません。

## EK-RA8P1

| 項目 | reference profile |
| --- | --- |
| SD transport | SPI接続microSD |
| hardware crypto | RSIP-E50D |
| accelerator | Ethos-U55 |
| Sentinel profile | 最適化済みRelease |

Debug buildは機能確認や診断には利用できますが、timing分布がRelease buildとは異なるため、classification qualityの正式な評価対象には含まれません。

## STM32N6570-DK

| 項目 | reference profile |
| --- | --- |
| SD transport | SDMMC2、4-bit |
| transfer | IDMA＋IRQ |
| hardware crypto | SAES |
| accelerator | Neural-ART |
| Sentinel profile | 最適化済みRelease＋IDMA |

Debug buildおよびpolling transferは、機能確認や診断のために利用できますが、classification qualityの正式な評価対象には含まれません。

## portable boundary

両profileでは、`mtfs_block_device_t`、block registry、media state machine、diagnostics、time provider、sealed blob/model store、Sentinel coreを共通で使用します。一方、transport、crypto provider、NPU provider、boardのIRQ/pin設定はtargetごとに異なります。

新しいportが共通APIに適合していても、それだけで上記reference profileと同等のclassification qualityが得られるとは限りません。
