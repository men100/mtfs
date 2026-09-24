# Source integration guide

microT-FSは、target HAL、OS、generated codeとの接続を利用者のproject側で行えるよう、固定された`.a`/`.lib`ではなく`src/`以下のsource codeを配布します。

sourceとして組み込むことで、未使用featureをcompile時に除外できるほか、targetごとのHAL type、pin設定、linker section、cache policyなどをprojectのbuild設定と合わせて確認できます。

## 最小構成

FatFsから1つのphysical block deviceを使用する最小構成では、次のtranslation unitをcompile対象へ追加します。headerはcompile対象には追加せず、include pathを通して参照します。

```text
src/block/mtfs_block_device.c
src/block/mtfs_block_diagnostics.c
src/block/mtfs_block_registry.c
src/core/mtfs_media.c
src/fatfs/ff.c
src/fatfs/mtfs_diskio.c
```

`mtfs_block_diagnostics.c`は、`MTFS_ENABLE_DIAGNOSTICS=0`の場合でも`NOT_SUPPORTED`を返すpublic API stubを提供します。そのため、本書では常に上記のsourceに含めています。diagnosticsの既定値は`1`です。

これらの共通sourceに加えて、使用するphysical block portを1つ追加します。

| target / port        | compile対象へ追加するsource                                                                                    | 備考                                      |
| -------------------- | ------------------------------------------------------------------------------------------------------- | --------------------------------------- |
| EK-RA8P1 SPI microSD | `src/ports/ra_fsp/sd_spi/mtfs_ra_sd_spi.c`、`mtfs_ra_sd_spi_deadline.c`                                  | FSP SPI/DMAC/timer/external IRQの設定が別途必要 |
| STM32 SDMMC          | `src/ports/stm32_cube/sdmmc/mtfs_stm32_sdmmc.c`、`src/ports/stm32_cube/common/mtfs_stm32_hal_timebase.c` | STM32 HAL SDMMC/DMA/IRQの設定が別途必要         |

既存board向けの初期化helperも使用する場合は、RAでは`src/ports/ra_fsp/boards/ek_ra8p1/mtfs_ra8p1_platform.c`、STでは`src/ports/stm32_cube/boards/stm32n6570_dk/mtfs_stm32n6570_dk_platform.c`を追加します。

独自boardを使用する場合は、同等のHAL初期化、block config、Card Detect callbackをapplication側で実装してください。

compile-time設定によって、さらに次のsourceが必要になります。

| 設定                                            | 追加source                                                               |
| --------------------------------------------- | ---------------------------------------------------------------------- |
| `MTFS_FF_ENABLE_LFN=1`                        | `src/fatfs/ffunicode.c`                                                |
| `MTFS_FF_FS_NORTC=0`                          | `src/core/mtfs_time.c`、`src/fatfs/mtfs_fattime.c`、下表のtarget RTC source |
| `MTFS_FF_FS_REENTRANT=1` + microT-Kernel      | `src/os/microtkernel/mtfs_fatfs_mutex.c`                               |
| `MTFS_FF_FS_REENTRANT=1` + POSIX Host         | `src/os/host/mtfs_fatfs_mutex_posix.c`                                 |
| microT-KernelのCard Detect/media event service | `src/os/microtkernel/mtfs_media_service.c`                             |

microT-FSが対応するLFN modeは、caller stackを使用する`FF_USE_LFN=2`です。この構成では`src/fatfs/ffsystem.c`を追加する必要はありません。

include pathの基点は`src/`です。上記sourceが置かれている各subdirectoryについても、必要に応じてprojectのinclude pathへ追加してください。

`src/mtfs.h`、`mtfs_config.h`、`mtfs_error.h`、`mtfs_types.h`は`src/`を基点として参照されます。また、すべてのtranslation unitで同じ`MTFS_*` macro設定を使用してください。

FatFsはR0.16 patch 2をrepositoryへ直接収録しています。upstreamの来歴については[`UPSTREAM.md`](../src/fatfs/UPSTREAM.md)、microT-FSで加えた変更については[`CHANGES.mtfs.md`](../src/fatfs/CHANGES.mtfs.md)を参照してください。

board固有の処理をupstreamの`ff.c`へ追加せず、`mtfs_diskio.c`を介してBlock Device APIへ接続します。

## feature別source

| feature                                      | 追加directory / source                                                                       |
| -------------------------------------------- | ------------------------------------------------------------------------------------------ |
| microT-Kernel                                | `src/os/microtkernel/`、利用targetの`mtk3_bsp2` source                                         |
| POSIX Host mutex                             | `src/os/host/mtfs_fatfs_mutex_posix.c`                                                     |
| EK-RA8P1 RTC                                 | `src/ports/ra_fsp/rtc/mtfs_ra_rtc.c`                                                       |
| STM32 RTC                                    | `src/ports/stm32_cube/rtc/mtfs_stm32_rtc.c`                                                |
| wrapped-key record / FatFs保存を使用するapplication | `src/extensions/security/wrapped_key/mtfs_wrapped_key_record.c`、`mtfs_wrapped_key_fatfs.c` |

`src/os/microtkernel/`と利用target用の`mtk3_bsp2`のように、project側でdirectory全体をlinked resourceとして取り込む構成も利用できます。

一方、複数targetのportやoptional extensionを含む`src/`全体をprojectへ取り込む場合は、使用しないdirectoryをbuild対象から除外してください。

### Sealed model

`MTFS_ENABLE_SEALED_MODEL=1`を有効にする場合は、まずtarget非依存の次のsourceを追加します。

```text
src/extensions/security/sealed_blob/mtfs_secure_zero.c
src/extensions/security/sealed_blob/mtfs_sealed_format.c
src/extensions/security/sealed_blob/mtfs_sealed_blob.c
src/extensions/ai/model_store/mtfs_model_store.c
src/fatfs/mtfs_sealed_reader_fatfs.c
```

さらに、hardware-backed key operationとAES-256-GCMを提供するtarget providerを1つ選択します。

| target provider        | compile対象へ追加するsource                                                                                                              | 外部依存・設定                                                                                                            |
| ---------------------- | --------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| EK-RA8P1 RSIP          | `src/ports/ra_fsp/crypto/mtfs_ra8p1_rsip_provider.c`、`mtfs_ra8p1_ospi_key_store.c`                                                | FSP RSIP/PSA Crypto、OSPI、provider lock/unlock callback                                                             |
| STM32N6570 SAES / DHUK | `src/ports/stm32_cube/crypto/mtfs_stm32_saes.c`、`mtfs_stm32_saes_provider.c`、`mtfs_stm32_nor_key_store.c`、`mtfs_stm32n6570_nor.c` | STM32 HAL CRYP/XSPI、board BSP NOR、provider lock/unlock callback。SDMMC portを使用しない構成でも`mtfs_stm32_hal_timebase.c`が必要 |

独自targetでは既存providerのsourceを追加するのではなく、`mtfs_crypto_provider_t`の`open_fleet_key`、`open_model_key`、`decrypt_chunk`、`close_key`を実装します。

実装するproviderは、plaintextのfleet key/model keyをprovider外部へ渡さないことに加え、authentication失敗時にplaintextを公開しないこと、使用したhandleをcloseすること、一時bufferをzeroizeすることなど、既存providerと同じcontractを満たす必要があります。

### Storage Sentinel

必要なsourceは、使用する機能によって異なります。

| 使用する機能                                   | macro                                            | compile対象へ追加するsource                                                                 |
| ---------------------------------------- | ------------------------------------------------ | ------------------------------------------------------------------------------------ |
| passive observation / feature生成          | `MTFS_ENABLE_STORAGE_SENTINEL=1`                 | `src/sentinel/mtfs_sentinel.c`、`mtfs_sentinel_baseline.c`、`mtfs_sentinel_observer.c` |
| CPU bundle parse / fixed-point inference | 上記に加えて`MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE=1` | `src/sentinel/mtfs_sentinel_inference.c`、`mtfs_sentinel_sha256.c`                    |
| sealed Sentinel bundle                   | 上記に加えて`MTFS_ENABLE_SEALED_MODEL=1`               | `src/sentinel/mtfs_sentinel_sealed_adapter.c`と、Sealed model節のsource/provider         |
| target NPU inference                     | `MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE=1`       | `src/sentinel/mtfs_sentinel_npu_provider.c`と、下表のtarget provider                      |

passive observationまたはCPU inferenceのみを使用する場合は、target NPU providerは必要ありません。

NPU inferenceを使用する場合のみ、次のtarget固有sourceとvendor runtimeを追加します。

| target NPU provider       | compile対象へ追加するsource                                                                                                                                                                   | 追加要件                                                                                                   |
| ------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| EK-RA8P1 TFLM / Ethos-U55 | `src/ports/ra_fsp/boards/ek_ra8p1/mtfs_ra8p1_sentinel_npu.c`、`src/ports/ra_fsp/npu/mtfs_ra8p1_tflm_ethosu.cpp`、`mtfs_ra8p1_ethosu_hooks.c`                                             | FSP Ethos-U、TensorFlow Lite Micro、providerへのアクセスの直列化、cache/arena lifecycle                             |
| STM32N6570 Neural-ART     | `src/ports/stm32_cube/boards/stm32n6570_dk/mtfs_stm32n6570_sentinel_npu.c`、`src/ports/stm32_cube/npu/mtfs_stm32n6_neural_art.c`、`mtfs_stm32n6_async_wait.c`、`mtfs_stm32n6_aton_osal.c` | ST Neural-ART middleware、generated relocatable runtime、`LL_ATON_*` build設定、NPU IRQ/cache/linker region |

target providerを追加するだけでは、application側のlifecycleは完結しません。

application側で、認証済みbundleのload、runtime policy、arena、lock、timeout、およびproviderのopen/infer/close処理を組み合わせる必要があります。

RAの実装例は[`apps/sentinel-lab/targets/ek_ra8p1/application`](../apps/sentinel-lab/targets/ek_ra8p1/application)、STの実装例は[`apps/sentinel-lab/targets/stm32n6570_dk/application`](../apps/sentinel-lab/targets/stm32n6570_dk/application)を参照してください。

独自のNPU targetを実装する場合は、`mtfs_sentinel_npu_provider_ops_t`の`inspect`、`install`、`infer`、`close`、`lock`、`unlock`、`zeroize`を実装し、実際のmemory/linker配置に対応したruntime region policyを設定します。

STのgenerated runtimeとruntimeを含むbundleはpublic repositoryに収録していないため、[Storage Sentinel guide](storage-sentinel.md#公開再現セット)に従って利用者の環境で生成してください。

IDEのlinked resourceを使用する場合は、repository内の相対pathを使用してください。また、sourceがbuild対象から除外されていないことをDebug/Releaseの両方で確認します。

FSP/Cubeのgenerated codeはIDE設定から再生成し、microT-FSのsourceと同じdirectoryへcopyして混在させないでください。generated HAL handleやpin symbolはboard bindingへ渡して使用します。

## allocationとlifecycle

block context、registry、media service、FatFs object、crypto/Sentinel arenaのstorageは呼び出し側が所有します。static storage、またはlifetimeが明確に管理されたstorageを使用してください。

core I/O pathではdynamic allocationを前提としません。また、callback contextとoperation tableは、deviceをregisterしている間は常に有効な状態に維持する必要があります。

基本的なlifecycleは次のとおりです。

```text
target/HAL initialize
  → block-device context initialize
  → block device register
  → FatFs mount
  → file access
  → open fileをclose
  → FatFs unmount
  → block device unregister
  → block-device context / target deinitialize
```

cleanupは、このlifecycleと逆の順序で行います。

途中で処理に失敗した場合も、その時点までに正常に完了した処理だけを逆順に戻します。具体的には、必要に応じてopen済みfileのclose、所有するtemporary fileの削除、unmount、IRQ/media serviceの停止、unregister、contextのdeinitを行います。

registryはdeviceの所有権を取得しません。そのため、unregisterしてもdeviceのmemoryはfreeされません。

最小構成の実装例は[`apps/simple`](../apps/simple/application/mtfs_simple.c)、より完全なmedia lifecycleについては[architecture](architecture/io-and-media.md)を参照してください。
