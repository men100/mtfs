# microT-FS 公開ドキュメント

microT-FSは、FatFsとmicroT-Kernel 3.0を接続し、交換可能なblock device、media lifecycle、診断機能、および任意で利用できるsealed model/Storage Sentinel機能を提供する組み込みストレージ基盤です。

## 目的別ガイド

| 目的 | 最初に読む文書 |
| --- | --- |
| まず全体像を把握したい | [アーキテクチャ概要](architecture/README.md)で、microT-FSの責務と利用範囲を確認してください。完全なgetting startedは今後追加する予定です。 |
| 既存projectへ組み込みたい | [全体レイヤー](architecture/overview.md)と[通常I/O / media lifecycle](architecture/io-and-media.md)を参照してください。 |
| 対応boardで動作させたい | [target profile](architecture/target-profiles.md)で正式に対応している構成を確認し、各target projectのREADMEを参照してください。 |
| 新しいblock device/boardへ移植したい | [通常I/O／media lifecycle](architecture/io-and-media.md)で、移植時に実装すべき境界を確認してください。完全なporting guideは今後追加する予定です。 |
| sealed modelを利用したい | [sealed model](architecture/sealed-model.md)で、鍵、認証、RAMのlifetime、および保証範囲を確認してください。 |
| Storage Sentinelを利用したい | [Storage Sentinel](architecture/storage-sentinel.md)で、passive observation、baseline、および判定処理の境界を確認してください。 |
| API仕様を確認したい | [microT-FS 公開APIリファレンス](https://men100.github.io/mtfs/index.html)を参照してください。 |

## 文書の範囲

ここでは、現在公開しているarchitectureとAPI契約について説明します。

boardのIDEへのimport/build/flash手順、pin接続表、console command一覧、完全なporting guide、実測performance値、memory使用量、学習手順、troubleshootingは対象外です。

対応状況や性能について推測で補足することはせず、現在のsource treeから確認できる範囲のみを扱います。

FatFs本体はupstream softwareであり、microT-FS固有のAPIとは区別して扱います。来歴については [`src/fatfs/UPSTREAM.md`](../src/fatfs/UPSTREAM.md)、microT-FS側での変更点については [`src/fatfs/CHANGES.mtfs.md`](../src/fatfs/CHANGES.mtfs.md) を参照してください。
