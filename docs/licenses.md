# Third-party software / licenses

この文書では、microT-FSで利用しているthird-party softwareと、それぞれのlicense/noticeの確認先を示します。法的助言を提供するものではありません。

実際の配布形態や使用するvendor toolchainに応じて、適用されるlicense条件とnoticeを利用者自身で確認してください。

`License`欄には、repository内に収録されているlicense本文、各fileのSPDX identifier、または使用しているversionのupstream licenseを記載しています。compilerやIDEなどrepositoryへ収録していないtoolについては、利用者が実際に取得したdistributionに付属するlicenseが最終的な確認先となります。

| 構成要素 | License | repository内での扱い | 確認先 |
| --- | --- | --- | --- |
| microT-FS | Apache-2.0 | source、document、project-owned artifactを公開 | [`LICENSE`](../LICENSE) |
| FatFs R0.16 patch 2 | FatFs License（BSD-style） | upstream sourceを保持し、microT-FS固有の変更を分離して管理 | [`LICENSE.txt`](../src/fatfs/LICENSE.txt)、[`UPSTREAM.md`](../src/fatfs/UPSTREAM.md)、[`CHANGES.mtfs.md`](../src/fatfs/CHANGES.mtfs.md) |
| microT-Kernel 3.0 BSP2 | T-License 2.2 | Git submoduleとして参照し、submoduleに含まれるnoticeは変更しない | [`mtk3_bsp2/README.md`](../mtk3_bsp2/README.md) |
| OpenSSL 3.x | Apache-2.0 | host側のsealed-model toolからCryptoへlink。source/binaryはrepositoryへ収録しない | [OpenSSL license](https://openssl-library.org/source/license/)、[`tools/sealed_model/CMakeLists.txt`](../tools/sealed_model/CMakeLists.txt) |
| TensorFlow / TFLite 2.18.1 | Apache-2.0。upstream distributionには、個別のnoticeが必要なthird-party codeを含む | Sentinelの学習とcanonical TFLiteの生成に使用。Python packageは収録せず、lock fileのみ公開 | [TensorFlow 2.18.1 `LICENSE`](https://github.com/tensorflow/tensorflow/blob/v2.18.1/LICENSE)、[`requirements-lock.txt`](../artifacts/storage_sentinel/environment/requirements-lock.txt) |
| NumPy 2.0.2 | BSD-3-Clause。wheel/distributionによってはbundled componentに追加licenseが適用される | dataset処理、学習、数値検証に使用。packageは収録せず、lock fileのみ公開 | [NumPy 2.0.2 `LICENSE.txt`](https://github.com/numpy/numpy/blob/v2.0.2/LICENSE.txt)、[`license-provenance.json`](../artifacts/storage_sentinel/license-provenance.json) |
| Arm Ethos-U Vela 5.1.0 | Apache-2.0 | RA Ethos-U向けの変換に使用。compiler packageは収録せず、生成したTFLiteを公開 | [Vela 5.1.0 package metadata](https://pypi.org/project/ethos-u-vela/5.1.0/)、[`vela-requirements-lock.txt`](../artifacts/storage_sentinel/environment/vela-requirements-lock.txt) |
| Arm CMSIS 6 | Apache-2.0 | RA target projectへsourceとlicenseを収録 | [`CMSIS_6/LICENSE`](../apps/simple/targets/ek_ra8p1/ra/arm/CMSIS_6/LICENSE) |
| Mbed TLS | Apache-2.0 OR GPL-2.0-or-later | security-enabledなRA target projectへsourceとlicenseを収録 | [`mbedtls/LICENSE`](../apps/key-provision/targets/ek_ra8p1/ra/arm/mbedtls/LICENSE) |
| Renesas FSP/RA board support | FSP由来fileはBSD-3-Clause。その他のupstream由来fileは各fileのSPDXに従う | RA target projectへFSP/generated sourceを収録し、copyright/SPDX headerを保持 | [`ra/fsp`](../apps/simple/targets/ek_ra8p1/ra/fsp)、security-enabled projectの[`ra/fsp`](../tests/targets/ek_ra8p1/ra/fsp) |
| CMSIS Core/STM32N6 device support | Apache-2.0 | STM32 target向けのexternal sourceを収録 | [CMSIS `LICENSE`](../external/stm32_cube/stm32n6570_dk/Drivers/CMSIS/LICENSE)、[STM32N6 device `LICENSE.txt`](../external/stm32_cube/stm32n6570_dk/Drivers/CMSIS/Device/ST/STM32N6xx/LICENSE.txt) |
| STM32N6 HAL driver | BSD-3-Clause（package外で個別に受領した場合。package内では`Package_license`を優先） | STM32 target向けのexternal sourceを収録 | [`STM32N6xx_HAL_Driver/LICENSE.txt`](../external/stm32_cube/stm32n6570_dk/Drivers/STM32N6xx_HAL_Driver/LICENSE.txt) |
| ST Neural-ART NPU middleware | SLA0104 Rev.1（package外で個別に受領した場合。package内では`Package_license`を優先） | interface/runtime sourceの一部を`external/`へ収録。ST生成runtimeを含むbundle、`SENTINEL.MTF`、firmwareは公開しない | [`Middlewares/ST/AI/Npu/LICENSE.txt`](../external/stm32_cube/stm32n6570_dk/Middlewares/ST/AI/Npu/LICENSE.txt)、[`license-provenance.json`](../artifacts/storage_sentinel/license-provenance.json) |
| e² studio/Renesas FSP distribution、STM32CubeIDE/CubeMX/CubeProgrammer、ST Edge AI tools | 各distributionに付属するvendor license/EULA | IDE、compiler、programmer、code generator自体はrepositoryへ収録しない | 利用者が取得したversionのinstaller、`Package_license`、About/license表示 |
| 公開Storage Sentinel dataset、canonical TFLite、RA bundle/sealed package | Apache-2.0（repository license） | project-owned data/codeから生成した公開artifact。RA sealed packageは公開test key専用 | [`license-provenance.json`](../artifacts/storage_sentinel/license-provenance.json)、[`SHA256SUMS`](../artifacts/storage_sentinel/SHA256SUMS) |

公開しているStorage Sentinel dataset、project-owned training code、canonical TFLite、RA bundleについては、[`license-provenance.json`](../artifacts/storage_sentinel/license-provenance.json)に由来を記録しています。各artifactのhashは[`SHA256SUMS`](../artifacts/storage_sentinel/SHA256SUMS)で確認できます。

ST Neural-ARTを含むartifactは、現在のpublic repositoryには収録していません。公開しているrecipeは、利用者の環境でartifactを生成するための手順を示すものです。

vendor licenseに基づく再配布可否や配布条件について、microT-FS側で独自の保証は行いません。特にpackageに含まれるcomponentでは、個別directoryのlicenseよりもpackage全体に適用される`Package_license`が優先される場合があります。

FSP、STM32Cube、Edge AI tool、external loader、generated runtimeについては、実際に取得したdistributionに付属するlicenseと配布条件を確認してください。

license本文、upstream notice、submodule noticeは、機能変更を目的として編集しません。

公開test keyはsoftware licenseとは別にsecurity上の注意が必要です。[Security guide](security.md#公開test-key)に記載された注意事項に従ってください。
