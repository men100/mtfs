# Doxygen API reference

repository rootにある`Doxyfile`を使って、日本語UIのC向けHTML API referenceを生成できます。

Doxygenはproduction buildとは独立しているため、Doxygenを導入していなくてもfirmware/host buildには影響しません。公開用のHTMLはGitHub Actionsで生成し、GitHub Pagesへdeployします。

## GitHub Pagesへの公開

`.github/workflows/doxygen-pages.yml`は、`main`へのpush時、またはGitHub Actions画面から手動実行したときに、次の処理を行います。

1. Ubuntu runnerへDoxygenを導入
2. `doxygen Doxyfile`を実行
3. `build/doxygen/html`をPages artifactとしてupload
4. `github-pages` environmentへdeploy

生成したHTMLはrepositoryへcommitしません。

初回のみ、repositoryのSettings > PagesでSourceにGitHub Actionsを選択してください。公開URLはworkflowのdeploy jobから確認できます。

## ローカルでの確認

Doxygen 1.9以降を用意し、repository rootで次のcommandを実行します。

```console
doxygen Doxyfile
````

生成されたHTMLは`build/doxygen/html/index.html`から確認できます。

`build/doxygen/`は`.gitignore`の対象になっているため、生成HTMLがrepositoryへ追加されることはありません。また、Doxygenのwarningをerrorとして扱う設定にしているため、commandが正常終了した場合はwarning/errorともに0件です。

## API group

* Core Types and Errors
* Block Device
* Block Registry
* Removable Media
* Diagnostics
* Time and RTC
* FatFs Integration
* microT-Kernel Integration
* Sealed Blob and Crypto Provider
* Model Store
* Storage Sentinel
* Target Ports

## headerの分類

### Portable public API

`mtfs.h`から直接、またはfeature guardを通じて公開されるheaderです。

* `mtfs_config.h`、`mtfs_error.h`、`mtfs_types.h`
* `block/mtfs_block_device.h`、`mtfs_block_registry.h`、`mtfs_block_diagnostics.h`
* `core/mtfs_media.h`、`core/mtfs_time.h`
* `extensions/security/sealed_blob/mtfs_*.h`
* `extensions/ai/model_store/mtfs_model_store.h`
* `sentinel/mtfs_sentinel.h`、`mtfs_sentinel_baseline.h`、`mtfs_sentinel_observer.h`、`mtfs_sentinel_inference.h`、`mtfs_sentinel_sealed_adapter.h`

`mtfs_sentinel_npu_provider.h`と`mtfs_sentinel_sha256.h`はumbrella headerから直接includeされませんが、target-neutralなprovider/integrity boundaryとしてapplicationとtarget adapterの両方から利用されるため、公開APIとして扱います。

### Public integration API

利用者が対応environment上でcontextを直接生成・初期化して使用するadapterです。

portableな`mtfs.h`には含まれないため、必要なvendor/OS headerとあわせて個別にincludeします。

* `os/microtkernel/mtfs_media_service.h`
* `fatfs/mtfs_sealed_reader_fatfs.h`
* `extensions/security/wrapped_key/mtfs_wrapped_key_record.h`、`mtfs_wrapped_key_fatfs.h`
* `ports/host/mtfs_host_block_file.h`
* RA: `mtfs_ra_sd_spi.h`、`mtfs_ra_rtc.h`、`mtfs_ra8p1_rsip_provider.h`、`mtfs_ra8p1_ospi_key_store.h`、`mtfs_ra8p1_tflm_ethosu.h`
* ST: `mtfs_stm32_sdmmc.h`、`mtfs_stm32_rtc.h`、`mtfs_stm32_saes_provider.h`、`mtfs_stm32_nor_key_store.h`、`mtfs_stm32n6_neural_art.h`

### Internal/生成対象外

次のheaderは、現在の利用箇所と責務を踏まえ、正式なpublic APIとしては扱いません。

* `*_internal.h`: common implementation内部で共有するhelper
* `mtfs_ra_sd_spi_deadline.h`、`mtfs_ra_sd_spi_protocol.h`: RA SPI port内部で使用するprotocol/deadline helper
* `mtfs_stm32_sdmmc_wait_policy.h`: ST SDMMC port内部で使用するwait policy helper
* `mtfs_stm32_hal_timebase.h`、`mtfs_stm32n6_async_wait.h`、`mtfs_stm32n6_aton_osal.h`、`ll_aton_osal_user_impl.h`: HAL/Neural-ARTとの接続に使用するglue code
* `mtfs_ra8p1_ethosu_hooks.h`、`mtfs_ra8p1_ethosu_instance.h`: board/runtimeとの接続に使用するglue code
* board `*_board_config.h`、`*_platform.h`、`*_sentinel_npu.h`、`mtfs_ra8p1_vector_cache.h`: 対応project内で使用するboard-localな構成要素
* `mtfs_stm32_saes.h`、`mtfs_stm32n6570_nor.h`: public provider/key-store adapterの内部で使用するdevice-specific primitive
* `mtfs_fatfs_mutex.h`: FatFsが必要とするsymbolを提供するためのbuild integrationであり、applicationから直接呼び出すAPIではない
* `app/mtfs_app_log.h`: bundled application向けのhelperであり、library APIではない
* FatFs upstreamの`ff.h`、`diskio.h`、`ffconf.h`
* `external/`、`mtk3_bsp2/`、vendor/generated code

この分類は、現在のinclude関係、application/testからの直接利用状況、context ownership、およびtarget adapterの責務に基づいています。

internal headerについては、将来の互換性を保証しません。
