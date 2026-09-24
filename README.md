# microT-FS

microT-FSは、microT-Kernel 3.0向けにFatFs、removable block device、media lifecycle、RTC、diagnosticsを統合した組み込みstorage基盤です。

組み込みprojectでは、原則として`src/`以下のsource codeを取り込んで使用します。

標準構成はFatFsとportableなBlock Device APIです。hardware-backed keyを使用するsealed modelと、storage I/Oを監視するStorage Sentinelは、必要に応じて有効化できるoptional featureです。

## 主な機能

* FatFs R0.16 patch 2とportableなBlock Device API/registry
* SD cardの挿入・取り外し、debounce、media generationを扱うlifecycle
* RTCを使用したFAT timestampと、version付きstructured diagnostics
* EK-RA8P1向けSPI microSD port
* STM32N6570-DK向けSDMMC2 4-bit IDMA＋IRQ portとpolling fallback
* optionalなsealed model、hardware-backed wrapped key、Storage Sentinel
* Linux/WSL2上で動作するHost testとsealed-model Host tool

## まず何をしたいか

| 目的                                  | 開始地点                                                                     |
| ----------------------------------- | ------------------------------------------------------------------------ |
| まずHostでbuild/testしたい                | [Getting Started - Hostで試してみる](docs/getting-started.md#hostで試してみる)       |
| EK-RA8P1で動作させたい                     | [Getting Started - EK-RA8P1](docs/getting-started.md#ek-ra8p1)           |
| STM32N6570-DKで動作させたい                | [Getting Started - STM32N6570-DK](docs/getting-started.md#stm32n6570-dk) |
| 自分のprojectへsourceを組み込みたい            | [Source integration guide](docs/integration.md)                          |
| sample/test/provisioning toolを操作したい | [Application / console manual](docs/applications.md)                     |
| 新しいboardやstorage deviceへ移植したい       | [New port guide](docs/porting.md)                                        |
| 問題の原因を切り分けたい                        | [Troubleshooting](docs/troubleshooting.md)                               |

repositoryを取得した後は、microT-Kernel 3.0 BSP2を含むsubmoduleを初期化してください。

```console id="85qzsg"
git submodule update --init --recursive
```

Hostでの最小動作確認は、LinuxまたはWSL2で実行できます。

```console id="cwgmmr"
cmake -S tests/host -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --parallel
ctest --test-dir build/host --output-on-failure
```

必要なpackage、target projectのimport、Debug実行までの手順については[Getting Started](docs/getting-started.md)を参照してください。

## 対応環境とreference profile

| 環境            | 主な用途                                                  | storage transport             |
| ------------- | ----------------------------------------------------- | ----------------------------- |
| EK-RA8P1      | target application、test、sealed model、Storage Sentinel | Pmod MicroSD、SPI              |
| STM32N6570-DK | target application、test、sealed model、Storage Sentinel | on-board microSD、SDMMC2 4-bit |
| Linux / WSL2  | Host test、Host tool                                   | file-backed test device       |

Storage Sentinelのclassification qualityを評価する正式なreference profileは、次の2つです。

* EK-RA8P1: optimized Release/SPI/Ethos-U55
* STM32N6570-DK: optimized Release/SDMMC2 IDMA＋IRQ/Neural-ART

Debug buildとSTM32のpolling fallbackも、機能確認やdiagnosticsには利用できます。ただし、Storage Sentinelのclassification qualityを正式に評価する条件には含まれません。

これは、通常のFatFs/block storage機能をDebug buildで利用できないという意味ではありません。

詳しくは[target profile](docs/architecture/target-profiles.md)を参照してください。

## Repository layout

| path                          | 内容                                                                     |
| ----------------------------- | ---------------------------------------------------------------------- |
| `src/`                        | portable core、FatFs、OS adapter、target port、optional extension          |
| `apps/`                       | simple application、fleet-key provisioner、Sentinel Lab                  |
| `tests/host/`                 | Host unit/integration testとC/C++ public-header compile                 |
| `tests/targets/`              | EK-RA8P1/STM32N6570-DK向けtarget test application                        |
| `tools/`                      | sealed-model Host tool、STM32 image/deployment helper、Sentinel生成・監査tool |
| `artifacts/storage_sentinel/` | dataset、model、numerical corpus、provenance、公開監査記録                       |
| `docs/`                       | architecture、導入・運用・security・porting資料                                  |
| `mtk3_bsp2/`                  | microT-Kernel 3.0 BSP2 submodule                                       |

組み込み時に必要なsourceは、使用するfeatureによって異なります。

`src/`全体を無条件にbuildするのではなく、[Source integration guide](docs/integration.md)と[Configuration reference](docs/configuration.md)で必要なsourceとmacroを確認してください。

## Documentation

### 導入と通常利用

* [公開ドキュメント一覧](docs/README.md)
* [Getting Started](docs/getting-started.md)
* [Board configuration](docs/board-configuration.md)
* [Source integration guide](docs/integration.md)
* [Configuration reference](docs/configuration.md)
* [Storage operations](docs/storage-operations.md)
* [Application / console manual](docs/applications.md)
* [API Reference](https://men100.github.io/mtfs/index.html)

### Security、AI、deployment

* [Sealed Model / Security](docs/security.md)
* [Storage Sentinel](docs/storage-sentinel.md)
* [STM32 deployment](docs/stm32-deployment.md)

### 設計、移植、reference

* [Architecture](docs/architecture/README.md)
* [New port guide](docs/porting.md)
* [Troubleshooting](docs/troubleshooting.md)
* [Performance / resource reference](docs/performance.md)
* [Third-party software / licenses](docs/licenses.md)

## Dataとsecurityに関する注意

* mount失敗だけを理由にmediaを自動formatすることはありません。検証には、既存dataを失っても問題のない専用cardを使用してください。
* repositoryに含まれる公開test keyとtest packageはdemo/test専用です。production用途には使用しないでください。
* production fleet keyをsource、command line、shell history、console log、removable SDへ保存しないでください。deviceへの登録には専用の[`apps/key-provision`](docs/applications.md#appskey-provision)を使用します。
* STM32 Neural-ART generated runtimeを含むartifactには、ST softwareのlicense条件が適用されます。公開repositoryにはruntimeを含む`SENTINEL.MTF`を収録していません。利用者が正規に取得したtoolchainを使用し、自身の環境で生成してください。

security boundary、keyの管理責任、公開artifactの範囲については[Sealed Model / Security guide](docs/security.md)および[Storage Sentinel guide](docs/storage-sentinel.md#raとstで公開artifactが異なる理由)を参照してください。

## License

microT-FSは[Apache License 2.0](LICENSE)で提供します。

FatFs、microT-Kernel 3.0 BSP2、vendor HAL/generated code、toolchain、generated artifactには、それぞれ個別のlicense条件が適用されます。

各構成要素のlicenseとrepository内での扱いについては[Third-party software / licenses](docs/licenses.md)を参照してください。
