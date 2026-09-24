# microT-FS 公開ドキュメント

このdirectoryには、microT-FSの公開ドキュメントをまとめています。microT-FSを導入する場合は、まず[Getting Started](getting-started.md)から始めてください。

## 目的別ガイド

| 目的                                            | 文書                                                                                                 |
| --------------------------------------------- | -------------------------------------------------------------------------------------------------- |
| まずHostで試してみたい                                 | [Getting Started - Hostで試してみる](getting-started.md#hostで試してみる)                                                   |
| EK-RA8P1/STM32N6570-DKで動作させたい                 | [Getting Started - EK-RA8P1](getting-started.md#ek-ra8p1)、[Getting Started - STM32N6570-DK](getting-started.md#stm32n6570-dk) |
| 配線やpin assignmentを確認したい                       | [Board configuration](board-configuration.md)                                                      |
| 自分のprojectへsourceを組み込みたい                      | [Source integration](integration.md)                                                               |
| compile-time設定を確認したい                          | [Configuration reference](configuration.md)                                                        |
| FatFs、hotplug、RTC、diagnostics、benchmarkを利用したい | [Storage operations](storage-operations.md)                                                        |
| sample/test/provisioning/Sentinel Labを操作したい   | [Application / console manual](applications.md)                                                    |
| sealed modelとkeyのsecurity boundaryを確認したい      | [Sealed Model / Security](security.md)                                                             |
| Storage Sentinelを評価・再現したい                     | [Storage Sentinel](storage-sentinel.md)                                                            |
| STM32N657へimageを書き込みたい                        | [STM32 deployment](stm32-deployment.md)                                                            |
| 新しいboard/storage deviceへ移植したい                 | [New port guide](porting.md)                                                                       |
| 問題の原因を切り分けたい                                  | [Troubleshooting](troubleshooting.md)                                                              |
| performanceやRAM/ROM使用量の参考値を確認したい              | [Performance / resource reference](performance.md)                                                 |
| licenseとartifactの公開範囲を確認したい                   | [Third-party software / licenses](licenses.md)                                                     |
| 全体のarchitectureを把握したい                         | [Architecture](architecture/README.md)                                                             |
| 公開C APIの仕様を確認したい                              | [API Reference](https://men100.github.io/mtfs/index.html)                                          |

## 文書の読み方

architecture文書ではcomponent間の責務や設計理由を説明し、各利用ガイドでは現在の設定や具体的な操作手順を説明します。

FatFsのupstream情報については[`UPSTREAM.md`](../src/fatfs/UPSTREAM.md)、microT-FSで加えた変更については[`CHANGES.mtfs.md`](../src/fatfs/CHANGES.mtfs.md)を参照してください。

performance値は保証値ではなく、記載されたboard、card、build、commitで測定したreference resultです。SD cardの個体差、断片化、温度、wear leveling、内部GCなどによって変動します。
