# Getting Started

## Hostで試してみる

### 対応環境と依存関係

Host testとsealed-model host toolは、LinuxまたはWSL2での利用を前提としています。

Host testには、Git、CMake 3.16以降、C/C++ compiler、makeまたはNinja、pthreadが必要です。sealed-model toolには、さらにC++17 compilerとOpenSSL 3.x development packageが必要です。

Storage Sentinelの再現toolはPython 3.10系を基準としており、固定されたdependencyは[`requirements-lock.txt`](../artifacts/storage_sentinel/environment/requirements-lock.txt)と[`vela-requirements-lock.txt`](../artifacts/storage_sentinel/environment/vela-requirements-lock.txt)に記載しています。

### Clone、configure、build、test

```console
git clone --recurse-submodules https://github.com/men100/mtfs.git
cd mtfs
git submodule update --init --recursive
cmake -S tests/host -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --parallel
ctest --test-dir build/host --output-on-failure
````

正常に完了すると、C/C++ public header、feature enabled test、read-only/non-reentrant/diagnostics/Sentinel、およびsealed-model feature-disabled構成を含む各compile targetがbuildされ、すべてのCTestがPASSします。

生成物は`build/host/`以下にのみ作成されます。

LFNを無効にした構成も、別のbuild directoryで検証できます。

```console
cmake -S tests/host -B build/host-no-lfn \
  -DMTFS_HOST_FF_ENABLE_LFN=0 -DCMAKE_BUILD_TYPE=Release
cmake --build build/host-no-lfn --parallel
ctest --test-dir build/host-no-lfn --output-on-failure
```

32-bit Arm向けpublic headerは、Arm GNU toolchainを指定することでCortex-M85/M55の両方に対してstrict compile checkを実行できます。

```powershell
pwsh tests/host/check_arm_public_headers.ps1 -ToolchainBin <arm-gnu-bin-directory>
```

sealed-model host toolは別途buildします。

```console
cmake -S tools/sealed_model -B build/sealed-model -DCMAKE_BUILD_TYPE=Release
cmake --build build/sealed-model --parallel
ctest --test-dir build/sealed-model --output-on-failure
```

`mtfs-keygen`、`mtfs-seal`、`mtfs-verify`、`mtfs-unseal`、`mtfs-test-package`は`build/sealed-model/`に生成されます。

Storage Sentinel toolについては、[Storage Sentinel guide](storage-sentinel.md#公開再現セット)を参照してください。

## EK-RA8P1

### 必要なもの

* Renesas EK-RA8P1、debug USB接続
* Digilent Pmod MicroSD Revision Aと、FAT12/16/32でformat済みのmicroSD card
* PMOD2への接続。詳細は[配線表と図](board-configuration.md#ek-ra8p1)を参照
* e² studio 2026-04.2、FSP 6.5.0、Arm GNU Toolchain 13.2.1

上記は、現行の`configuration.xml`で指定されているFSP/toolchain versionと、実際に動作確認を行った環境です。

### Import、build、flash

1. `git submodule update --init --recursive`を実行します。
2. e² studioで既存projectをimportします。最初の動作確認には`apps/simple/targets/ek_ra8p1`、console操作を行う場合は`tests/targets/ek_ra8p1`を選択します。
3. `configuration.xml`を開き、FSP code generationを実行します。
4. DebugまたはRelease configurationを選択し、clean buildします。
5. projectのdebug launchからboardへdownloadして起動します。
6. T-Monitor consoleへ接続します。外部terminalを使用する場合はprojectのVCOM設定に合わせ、115200 baud、8 data bits、no parity、1 stop bit、flow controlなしに設定します。

`apps/simple`は、mount、`HELLO.TXT`のwrite/read/verify/delete、unmountを自動的に実行し、最後に`[simple] overall=PASS`を表示します。

test applicationでは、まず`help`を実行し、続けて`test-roundtrip 1`を実行してください。各caseと最終結果がPASSになることを確認します。

microT-FSはSD cardを自動的にformatしません。mountに失敗した場合も、format機能を有効にして自動復旧させるのではなく、必要なdataを退避したうえで別の環境からcardを準備してください。

## STM32N6570-DK

### 必要なもの

* STM32N6570-DK、on-board microSD slot、ST-LINK USB接続
* FAT12/16/32でformat済みのmicroSD card
* STM32CubeIDE 2.1.1、STM32CubeMX 6.17.0、STM32Cube FW_N6 V1.3.0
* CubeIDE同梱のGNU Tools for STM32 14.3.rel1
* `apps/sentinel-lab`をbuildする場合のみ、ST Edge AI Core 4.0.1-20581（atonn 1.1.3）

UART consoleはST-LINK virtual COM portを使用し、115200 baud、8-N-1、flow controlなしに設定します。

### sentinel-lab向けNeural-ART middlewareの配置

ST Edge AI Coreをinstallしただけでは、`apps/sentinel-lab`のCubeIDE projectからmiddlewareは参照されません。新しいPCでbuildする前に、repository root（`mtfs`）から次の配置scriptを1回実行してください。`apps/simple`と`apps/key-provision`、`tests/targets/stm32n6570_dk`のbuildには不要です。

```powershell
.\tools\storage_sentinel\provision_st_neural_art_runtime.ps1 `
  -STEdgeAIRoot 'C:\ST\STEdgeAI\4.0'
```

`-STEdgeAIRoot`は**version directory**を指定します。例えば`C:\ST\STEdgeAI`は上位directoryなのでNG、`C:\ST\STEdgeAI\4.0`がOKです。指定先の直下に`Utilities\windows\stedgeai.exe`、`Utilities\windows\atonn.exe`、`Middlewares\ST\AI\Npu`があることを確認してください。install先が異なる場合は、同じ構成を持つdirectoryへ読み替えます。

scriptはtool versionを検査し、必要なNeural-ART source/headerに加えて`stm32n6xx_hal_rif.h`などのboard用driverを`external/stm32_cube/stm32n6570_dk/Middlewares/ST/AI/Npu`へ配置します。個々のheaderだけを手作業でcopyする必要はありません。この配置先はGit管理対象外なので、別PCのcheckoutでは再実行が必要です。

配置後は次が`True`になることを確認できます。

```powershell
Test-Path .\external\stm32_cube\stm32n6570_dk\Middlewares\ST\AI\Npu\Devices\STM32N6xx\stm32n6xx_hal_rif.h
```

CubeIDEでprojectをRefreshしてからClean Buildしてください。fileが存在してもinclude errorが残る場合は、`Appli` projectのlinked folder `STEdgeAINpu`が解決されているか確認します。projectをworkspace内へcopyしてimportすると、repositoryを起点にした相対linkが崩れます。

### Import、build、debug

1. `apps/simple/targets/stm32n6570_dk`、`apps/sentinel-lab/targets/stm32n6570_dk`、または`tests/targets/stm32n6570_dk`にあるroot、`FSBL`、`Appli`の各projectを同じworkspaceへimportします。`sentinel-lab`では先に上記のmiddleware配置を完了してください。
2. FSBLとAppliでDebug configurationを選択し、FSBL、Appliの順にclean buildします。
3. FSBL projectのdebug launchを開始します。launch configurationのload listに、FSBLとAppliの両方がbuild/download対象として含まれていることを確認してください。
4. `apps/simple`では`[simple] overall=PASS`が表示されることを確認します。test applicationでは`help`の後に`test-roundtrip 1`を実行し、FatFs、LFN、concurrency、diagnosticsがPASSし、SDMMC error/timeoutが0であることを確認してください。

このDebug launch手順は、初回の起動確認や機能debugを行うためのものです。

Storage Sentinelのclassification qualityやperformanceを評価する場合は、reference profileであるRelease buildを使用してください。

### Standalone imageとexternal flash

debuggerからdownloadせず、resetや電源再投入後もexternal flashからstandaloneで起動させる場合は、FSBL/Appliのraw binaryから**no-key trusted-header development image**を生成して書き込みます。

このimageは、cryptographic signingされたproduction imageではありません。

STM32CubeProgrammer、board用external loader、image生成、dry-run、書き込みaddress、verify、復旧手順については、[STM32 deployment](stm32-deployment.md)を参照してください。

Quick Startで使用するDebug launchでは、これらのtoolやexternal flashへの永続的な書き込みは必要ありません。

STM32N6570-DKでもSD cardを自動的にformatしません。既定の構成はSDMMC2 4-bit、IDMA＋IRQです。
