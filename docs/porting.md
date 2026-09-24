# New port guide

この文書では、microT-FSを新しいMCU、board、HAL、storage transportへ移植するための実装手順を説明します。

末尾のchecklistは実装を始めるための手順ではなく、移植完了後のDefinition of Doneとして使用してください。

APIの完全な宣言とfield定義については[API Reference](https://men100.github.io/mtfs/index.html)を参照してください。本書では、どのinterfaceをどの順序で実装し、それぞれの段階で何を確認するかを説明します。

## 1. 移植範囲を決める

最初からすべてのfeatureを移植する必要はありません。まず、必要な範囲を次のlevelから選択します。

| level               | 実現する機能                                | target側で実装する主な境界                                         | 前提となるlevel                       |
| ------------------- | ------------------------------------- | -------------------------------------------------------- | -------------------------------- |
| 1. Block storage    | sector read/writeとFatFs mount         | `mtfs_block_device_t`、transport/HAL、必要なlock              | なし                               |
| 2. Removable media  | Card Detect、debounce、hotplug recovery | GPIO/IRQ callback、monotonic clock、media worker接続         | Block storage                    |
| 3. RTC timestamp    | 実時刻のFAT timestamp                     | `mtfs_time_provider_t`、RTC lock、UNSET marker             | Block storage                    |
| 4. Sealed model     | hardware-backed keyと認証付きmodel load    | `mtfs_crypto_provider_t`、wrapped fleet-key store         | Block storage                    |
| 5. Storage Sentinel | passive observationとCPU inference     | diagnostics/timing、Sentinel application lifecycle        | Block storage                    |
| 6. Target NPU       | acceleratorによるSentinel inference      | `mtfs_sentinel_npu_provider_ops_t`、runtime memory policy | Storage Sentinel、通常はSealed model |

Level 1だけでも、通常のFatFs applicationを実行できます。

RTCを使用しない場合は`MTFS_FF_FS_NORTC=1`のままにします。sealed modelやStorage Sentinelを使用しない場合も、対応するfeature macroを無効のままにしてください。

また、「新しいboardで既存のtransport portを再利用する場合」と「新しいtransport driver自体を実装する場合」は分けて考える必要があります。

たとえば、STM32Cube HALのSDMMC portを別のSTM32 boardでも再利用できる場合、新規実装の中心となるのはpin、clock、RIF、Card Detectなどのboard bindingです。

HALやtransport自体が異なる場合は、Block Device adapterから実装します。

## 2. Portable boundaryを把握する

共通部分とtarget固有部分の境界は次のとおりです。

```text
application
  └─ FatFs
      └─ mtfs_diskio.c
          └─ block registry                 共通
              └─ mtfs_block_device_t        共通contract
                  └─ new transport port     target固有
                      └─ HAL / DMA / IRQ     vendor・board固有

Card Detect GPIO / IRQ
  └─ mtfs_media_notify_isr()
      └─ media state machine / worker       state machineは共通、接続はtarget固有

RTC / crypto / NPU
  └─ public provider interface              共通contract
      └─ target provider                    target固有
```

FatFs、block registry、media state machine、sealed format、model store、Sentinel coreへboard固有処理を追加しないでください。

HAL handle、pin、clock、IRQ、DMA/cache、hardware key、accelerator runtimeなどのtarget固有要素は、portまたはboard bindingの中へ閉じ込めます。

### 所有権の基本原則

microT-FSのcore I/O pathはdynamic allocationを前提としていません。

* applicationまたはportがcontext、device、diagnostics、arenaを確保します。
* registryやprovider registryは、登録されたobjectの所有権を取得しません。
* operation table、context、callback contextは、unregister/deinitが完了するまで有効な状態に維持する必要があります。
* callback内で非同期HAL処理を開始した場合でも、Block Device callback自体は完了または失敗が確定するまでcaller task contextで待機します。
* cleanupはresourceを取得した順序と逆に行います。途中で失敗した場合も、その時点までに正常に完了した処理だけを逆順に戻します。

allocationと共通lifecycleについては[Source integration](integration.md#allocationとlifecycle)、通常I/Oのdata flowについては[architecture](architecture/io-and-media.md)も参照してください。

## 3. 参照実装を選ぶ

新しいportに最も近い既存実装を1つ選びます。

まずheaderを読み、context、config、公開関数を把握してから、`.c`側のoperation実装を追うと理解しやすくなります。

| 用途                      | 最初に読むfile                                                                            | 次に読むfile                                                                                                      |
| ----------------------- | ------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------- |
| 最小Block Device contract | [`mtfs_block_device.h`](../src/block/mtfs_block_device.h)                            | [`mtfs_host_block_file.c`](../src/ports/host/mtfs_host_block_file.c)                                          |
| SPI接続SD、protocol制御      | [`mtfs_ra_sd_spi.h`](../src/ports/ra_fsp/sd_spi/mtfs_ra_sd_spi.h)                    | [`mtfs_ra_sd_spi.c`](../src/ports/ra_fsp/sd_spi/mtfs_ra_sd_spi.c)                                             |
| SDMMC、DMA/IRQ/cache     | [`mtfs_stm32_sdmmc.h`](../src/ports/stm32_cube/sdmmc/mtfs_stm32_sdmmc.h)             | [`mtfs_stm32_sdmmc.c`](../src/ports/stm32_cube/sdmmc/mtfs_stm32_sdmmc.c)                                      |
| board binding           | [`mtfs_ra8p1_platform.c`](../src/ports/ra_fsp/boards/ek_ra8p1/mtfs_ra8p1_platform.c) | [`mtfs_stm32n6570_dk_platform.c`](../src/ports/stm32_cube/boards/stm32n6570_dk/mtfs_stm32n6570_dk_platform.c) |
| mountと失敗時cleanup        | [`mtfs_simple.c`](../apps/simple/application/mtfs_simple.c)                          | [Source integration](integration.md#allocationとlifecycle)                                                     |
| Card Detectとhotplug     | [`mtfs_media.h`](../src/core/mtfs_media.h)                                           | [`mtfs_media_service.h`](../src/os/microtkernel/mtfs_media_service.h)                                         |

Host file portは、operation tableやerror処理を理解するための参照としては適しています。

ただし、組み込みtargetで必要となる耐久性、DMA、hotplugなどの参照実装ではありません。SPIを使用する場合はRA、SDMMC/DMAを使用する場合はSTの実装を主な参照にしてください。

## 4. Directoryと責務を分ける

新しいHAL/transportを追加する場合は、既存portと同様に次のような分離を推奨します。

```text
src/ports/<hal-family>/<transport>/
  mtfs_<target-or-hal>_<transport>.h
  mtfs_<target-or-hal>_<transport>.c

src/ports/<hal-family>/boards/<board>/
  mtfs_<board>_board_config.h
  mtfs_<board>_platform.h
  mtfs_<board>_platform.c
```

transport portは、HAL handle、signal callback、timeoutなどをconfigとして受け取る構成にし、固定されたpinやgenerated global symbolを直接参照しない形が望まれます。

board bindingでは、generated HAL/FSP object、pin、IRQ、clock、security/RIF設定とport configを接続します。

FSP/Cubeなどのgenerated fileは、IDE側の設定から再生成してください。

generated sourceをmicroT-FSのport directoryへcopyしたり、generated fileを直接編集して設定差分を隠したりしないでください。

既存portを別boardで再利用する場合は、transport directory自体を複製する前に、新しい`boards/<board>/` bindingを追加するだけで対応できないか検討してください。

## 5. Target projectの土台を作る

Block Device実装を始める前に、vendor HALとOSだけを使用して次を確認します。

1. Debug/Releaseの両方をwarning/errorなしでbuildできる。
2. clock、pinmux、電源、IRQ vectorをgenerated設定から再現できる。
3. monotonic millisecond counterをtask contextから取得できる。
4. 使用するkernelのmutex/event flagをtaskとIRQから正しいAPIで操作できる。
5. linker mapからRAM region、DMA可能領域、cacheable/non-cacheable属性を確認できる。

次に、[Source integration guide](integration.md#最小構成)に記載された共通sourceをprojectへ追加します。

include pathの基点は`src/`です。すべてのC/C++ translation unitへ同じ`MTFS_*` macroを供給してください。

この段階では新portがstubでも構いません。

public headerをC/C++の両方からcompileでき、共通sourceがtarget toolchainでbuildできる状態にします。

vendor固有のtypeを共通headerへ持ち込まず、target portのheader内だけに留めてください。

## 6. 最小Block Device portを実装する

### Context、config、device

portは通常、次の3種類のobjectを持ちます。

* **config**: HAL handle、signal callback、timeout、modeなど。init時にvalue copyするか、参照を保持する場合は必要なlifetimeを明記します。
* **context**: runtime state、geometry、kernel object、DMA buffer、last errorなど。通常は呼び出し側が静的に確保します。
* **`mtfs_block_device_t`**: operation table、context pointer、capability。context内へ埋め込んでも、独立したobjectとして確保しても構いません。

概念的な初期化形は次のようになります。

これはそのままcompileするtemplateではなく、各objectの関係を示すoutlineです。

```c
typedef struct my_block_context {
    my_block_config_t config;
    mtfs_block_device_t device;
    mtfs_block_geometry_t geometry;
    int initialized;
#if MTFS_ENABLE_DIAGNOSTICS
    mtfs_block_diagnostics_state_t common_diagnostics;
#endif
} my_block_context_t;

static const mtfs_block_device_ops_t my_block_ops = {
    my_initialize,
    my_status,
    my_read,
    my_write,
    my_sync,
    my_get_geometry,
    NULL /* trimを実装する場合だけcallbackを設定 */
};

mtfs_error_t my_block_context_init(
    my_block_context_t *context, const my_block_config_t *config)
{
    /* 引数検証、zero初期化、config copy、kernel object/HAL準備 */
    context->device.ops = &my_block_ops;
    context->device.context = context;
    context->device.capabilities = 0U;
#if MTFS_ENABLE_DIAGNOSTICS
    if (mtfs_block_diagnostics_attach(
            &context->device, &context->common_diagnostics) != MTFS_OK) {
        /* 取得済みresourceを逆順cleanup */
    }
#endif
    return mtfs_block_device_is_valid(&context->device)
        ? MTFS_OK : MTFS_ERROR_INVALID_ARGUMENT;
}
```

`context_init()`はport objectとkernel/HAL resourceを準備する処理です。

一方、`ops.initialize()`は実際にcard/mediaとの通信を開始し、geometryを確立する処理です。

この2つを混同すると、media交換後の再initializeや、途中で失敗した場合のcleanupが難しくなります。

### Operation contract

| operation      | 必須条件           | port側の責務                                                    |
| -------------- | -------------- | ----------------------------------------------------------- |
| `initialize`   | 常に必須           | mediaを検出・初期化し、成功した場合だけinitialized状態とgeometryを確定             |
| `status`       | 常に必須           | `INITIALIZED`、`MEDIA_PRESENT`、`WRITE_PROTECTED`を実際の状態に対応させる |
| `read`         | 常に必須           | 指定された`count`個のsectorをすべて転送するかerrorを返す                       |
| `write`        | read-only以外で必須 | 全sectorを転送し、write-protectやmedia removalを正しく報告               |
| `sync`         | 常に必須           | device/card内部を含め、保留中のwriteが完了したことを確認                        |
| `get_geometry` | 常に必須           | すべて0以外のsector size/count/erase block sizeを返す                |
| `trim`         | 任意             | capabilityを有効にした場合のみ必須                                      |

`mtfs_block_device_is_valid()`は、必須callbackとcapabilityの組み合わせを検査します。

read-only deviceでは`write`を省略できます。

trimをサポートしない場合は`MTFS_BLOCK_CAPABILITY_TRIM`を設定せず、`trim`を`NULL`にします。

`read`/`write`にはpartial successを表す戻り値がありません。

要求されたsectorの一部だけを転送した場合は成功を返さず、errorとして扱ってください。

HALが1回に扱えるblock数より大きなrequestを受けた場合は、port内部でchunkへ分割し、すべての転送が完了した場合だけ`MTFS_OK`を返します。

### 引数、range、overflow

public wrapperではNULL、0 count、geometryに基づくLBA範囲を検査します。

それでもport内部では、HALへ渡すbyte数、block数、address幅への変換でoverflowやtruncationが発生しないことを確認する必要があります。

特に、次の項目を転送開始前に検査してください。

* `lba + count`を直接加算せず、`count <= sector_count - lba`で範囲を判定する
* `lba * sector_size`と`count * sector_size`でoverflowしないこと
* HALのaddress/block-count型へ変換するときに値が表現範囲を超えないこと
* DMA alignment、DMA accessible range、bounce buffer容量
* media present、initialized、write protected

### Error mapping

vendor固有のstatusをそのままpublic APIへ返さず、安定した`mtfs_error_t`へmappingします。

元のHAL/kernel errorはtarget diagnosticsへ保存してください。

| 状況                             | 共通error                              |
| ------------------------------ | ------------------------------------ |
| pointer、0 count、設定値が不正         | `MTFS_ERROR_INVALID_ARGUMENT`        |
| mediaが物理的に存在しない                | `MTFS_ERROR_NO_MEDIA`                |
| 挿入中、未初期化、再初期化待ち                | `MTFS_ERROR_NOT_READY`               |
| write-protect検出                | `MTFS_ERROR_WRITE_PROTECTED`         |
| LBAまたは変換後addressが範囲外           | `MTFS_ERROR_OUT_OF_RANGE`            |
| 未対応operation/mode              | `MTFS_ERROR_NOT_SUPPORTED`           |
| HAL転送失敗、timeout、protocol error | 通常は`MTFS_ERROR_IO`。詳細はdiagnosticsへ保存 |
| lifecycle上許されない呼び出し            | `MTFS_ERROR_INVALID_STATE`           |

すべてのtimeoutを`NOT_READY`として扱ったり、media removalを一般的な`IO` errorへまとめたりすると、上位applicationのrecovery判断やdiagnosticsが不正確になります。

RA/STの既存portを参照し、どの段階でどのerrorへ分類しているか確認してください。

### Serializeとtimeout

同じdeviceに対するinitialize/status/read/write/sync/geometryへのアクセスは、port側のmutexで直列化するか、application側で明確な外部contractを定義して直列化します。

registryのregister/unregisterには内部同期がないため、I/O実行中にslotを変更しないようapplication側で管理してください。

非同期HALを使用する場合でも、operation callbackはevent flagなどを使って完了を待ちます。

absolute deadlineまでに完了しなければtransferをabortし、errorを返してください。

retryのたびにtimeoutを最初から数え直して総待ち時間が無制限に延びないよう、1つのoperation全体に対するdeadlineとして管理します。

### Level 1の到達条件

1. `mtfs_block_device_is_valid()`が真になる。
2. initialize後のstatusが`MEDIA_PRESENT | INITIALIZED`を返す。
3. geometryが実際のmediaと一致する。
4. 既知のsectorをreadし、期待する内容と一致する。
5. syncとdeinitを繰り返してもresource leakやstale IRQが発生しない。

利用者dataが保存されているmediaに対してraw write testを行わないでください。

## 7. RegistryとFatFsへ接続する

registryのslotはFatFsのphysical drive番号に対応します。

registryはdeviceの所有権を取得せず、参照だけを保持します。そのため、unregisterしてもdeviceのmemoryは解放されません。

基本的なlifecycleは次の順序です。

```text
board/HAL initialize
  → port context init
  → mtfs_block_registry_register(pdrv, device)
  → f_mount()                 diskio経由でblock initialize
  → file I/O
  → open file close
  → f_mount(NULL, volume, 0)  unmount
  → mtfs_block_registry_unregister(pdrv)
  → port context deinit
  → board/HAL cleanup
```

各段階が完了したかを示すflagを保持し、途中で失敗した場合は、その時点までに完了した処理だけを逆順に戻してください。

`f_mount()`が失敗しても、自動的にformatしないでください。

portの検証には、内容を失っても問題のない専用cardを使用します。

最初は次の順序で確認すると切り分けやすくなります。

1. block APIを使ったread-only sector read
2. 既存FAT cardのread-only mountとfile read
3. scratch cardで[`apps/simple`](applications.md#appssimple)のcreate/write/close/read/compare/delete
4. `test-roundtrip 1`
5. target profileで定義された通常round数

FatFsをreentrant構成にしていても、`f_mount()`などのvolume lifecycle operationはapplication側で直列化する必要があります。

必要なsourceとmacroについては[Source integration](integration.md)および[Configuration reference](configuration.md)を参照してください。

## 8. Removable mediaとCard Detectを追加する

`mtfs_media_context_t`はCard Detectのdebounceとmedia generationを管理します。

storage transportのinitialized状態を自動的に復旧するための仕組みではありません。

### ISRとtaskの分担

Card Detect ISRで行う処理は次の3つだけです。

1. raw GPIO levelを取得する。
2. `mtfs_media_notify_isr()`、またはmicroT-Kernelを使用する場合は`mtfs_media_service_notify_isr()`へ通知する。
3. transfer待機中のportへremoval hintを通知する。

ISR内では、GPIOの再読み取り、debounce待ち、mount/unmount、HAL close/reopen、memory allocation、application callbackを実行しません。

task contextでは`mtfs_media_process()`またはmedia service workerがdebounce後にsignalを再確認します。

media eventが確定した後、application側で必要なlifecycle処理を進めます。

### Remove

1. 新しいI/Oの開始を停止する。
2. transfer待機中であればwaiterを起床し、`NO_MEDIA`または`NOT_READY`で終了させる。
3. 可能な範囲でopen fileをcloseする。
4. unmount、unregister、port deinitを行う。

強制抜去前に完了していなかったwriteの成功や、filesystemの整合性は保証できません。

### Insert

1. debounceによってmediaが安定してpresentであることを確定する。
2. port context/transportを再度準備する。
3. initialize、register、mountをあらためて実行する。
4. 新しいfile objectをopenする。

mediaがpresentになったという通知だけで以前のinitialized状態へ戻したり、open済みfile objectを再利用したりしないでください。

media generationは以前のsessionと新しいsessionを区別するための値であり、自動復旧の仕組みではありません。

### Shutdown順序

終了時は、まずnotificationの受付を停止します。

その後、発生元IRQをdisable/unregisterし、最後にworker/media contextをdeinitします。

contextを破棄した後にlate IRQからnotifyされないことを確認してください。

state machineの詳細については[通常I/Oとremovable media](architecture/io-and-media.md#card-detect経路)を参照してください。

## 9. RTC providerを追加する

実際の時刻をFAT timestampへ記録する場合だけ`MTFS_FF_FS_NORTC=0`に設定し、`mtfs_time_provider_t`を実装します。

1. `get_local`、`set_local`、`get_status`、`clear`を実装します。
2. RTC accessが並行して発生する場合は、`lock`と`unlock`を両方設定します。
3. backup domainなどへUNSET markerを保存し、電源投入直後の未設定時刻をVALIDとして返さないようにします。
4. providerとcontextをstaticに保持し、registerからunregisterまで有効な状態に維持します。
5. local calendar timeを返します。timezone/DST/時刻同期はapplication側の責務です。

FATで表現できる年は1980～2107年で、timestampの分解能は2秒です。

RTCが未設定の場合やerror発生時に、もっともらしい時刻を合成して返さず、正しいstatusを返してください。

RTCを使用しないportではprovider stubを作る必要はありません。

既定の`MTFS_FF_FS_NORTC=1`では固定timestampを使用します。

操作手順については[RTCとtimestamp](storage-operations.md#rtcとtimestamp)を参照してください。

## 10. Diagnosticsとmonotonic timingを実装する

### Common block diagnostics

`MTFS_ENABLE_DIAGNOSTICS=1`では、port contextに`mtfs_block_diagnostics_state_t`を配置し、registration/I/O開始前に`mtfs_block_diagnostics_attach()`でdeviceへ接続します。

共通wrapperがoperation、success/failure、要求sector数、error分類、status、geometryを記録します。

diagnostics capabilityを有効にする場合は、実際にsnapshotを提供できる構成と一致させてください。

Storage Sentinelなどからsnapshot/resetが並行して実行されるportでは、必要に応じてlock付きattachを使用します。

### Target diagnostics

common snapshotだけではHAL/protocol固有の原因を確認できないため、RA/ST portと同様にtarget固有のsnapshotを用意することを推奨します。

最低限、次の情報を区別できるようにします。

* API version、struct size、validity mask、reset epoch
* initialization stageと現在のmode/state
* request、completion、failure、timeout、abort
* IRQ entry、completion/error callback、spurious/late IRQ
* last HAL/protocol/kernel/common error
* geometry、DMA/bounce/cache設定
* media removal notificationとwaiter wakeup

counterは最大値で飽和させ、wraparoundさせません。

resetではcounterとlast errorをclearしてreset epochを更新しますが、initialized、geometry、media stateなどのcached stateは維持します。

snapshotの取得やresetのために追加のmedia I/Oを発行したり、consoleへ出力したりしないでください。

### Monotonic clock

timeout、debounce、Storage Sentinelのtimingにはwall-clock RTCではなくmonotonic counterを使用します。

counterのwraparoundを考慮して差分を計算し、clock取得失敗と実際のI/O timeoutを別々にdiagnosticsへ記録してください。

snapshotの共通的な読み方については[Storage operations](storage-operations.md#diagnostics)を参照してください。

## 11. DMA、cache、IRQを実装する

DMAを有効にする前に、まずpollingまたは同期HALでBlock Device contractを満たすことを確認し、その後DMA/IRQ pathを追加すると原因を切り分けやすくなります。

ただし、pollingで得られた結果を正式なDMA reference profileの代わりに使用しないでください。

### Memory配置

* data buffer、descriptor、bounce bufferがDMA engineからアクセス可能なRAMに配置されていることをlinker mapで確認する
* context内bufferに必要なalignmentを型またはsectionで保証する
* user bufferがalignment/cache要件を満たさない場合はaligned bounce bufferを使用する
* 1回にbounceできるsector数に合わせてrequestを分割する
* stack上のbufferをDMAへ渡す場合は、必要なlifetimeとDMA accessible regionの両方を保証する

### Cache ownership

一般的なcache maintenanceの順序は次のとおりです。

最終的には、使用するMCU/HALのcache contractに従ってください。

* **write**: CPUが書き込んだdataをDMAへ渡す前にclean
* **read開始前**: dirty lineが後からDMAの結果を上書きしないよう、必要な範囲をclean/invalidate
* **read完了後**: CPUがdataを読む前にinvalidate

addressとlengthをcache line境界へ丸める場合は、同じcache line上にある無関係なdataを破壊しないよう、対象bufferが使用するcache line全体を専有できる配置にしてください。

### Completionとshutdown

IRQ callbackでは、設定したHAL handle/channelに対応するeventだけを処理します。

completion/errorをcontextへ記録し、event flagを使って待機中のtaskを起床します。

timeout時は、以降のIRQ受付を停止し、HAL transferをabortしてpending flagをclearした後でresourceを再利用してください。

late IRQやspurious IRQによって次のrequestが誤って完了扱いにならないよう、active stateまたはsequenceを確認します。

deinitでは、次のような順序を明確に設計します。

1. 新しいoperationの受付を停止
2. active transferを完了またはabort
3. IRQをdisable/unregister
4. kernel objectを削除
5. HALをclose

途中で処理に失敗した場合でも、再度deinitできる状態を定義しておいてください。

## 12. Sealed model対応を追加する

storage portが完成した後、必要な場合だけ`MTFS_ENABLE_SEALED_MODEL=1`を有効にします。

必要なsourceについては[Sealed model integration](integration.md#sealed-model)を参照してください。

新しいtargetでは、`mtfs_crypto_provider_t`の次のoperationを実装します。

* `open_fleet_key`
* `open_model_key`
* `decrypt_chunk`
* `close_key`

provider外部へ公開するのはopaque handleだけです。

plaintextのfleet key/model keyを返したり、通常firmware、log、ELF、removable mediaへ保存したりしないでください。

次の性質をnegative testで確認します。

* unknown key ID/versionをkey-not-foundとして拒否する
* envelopeまたはchunkのtag/ciphertext改変をauthentication failureとして拒否する
* authenticationが成功する前にplaintextを利用者へ公開しない
* 失敗時およびclose時にhandle、一時AAD/cipher/plain bufferをzeroizeする
* 並行accessを許可できないhardware engineへのアクセスをlockで直列化する

fleet keyのwrapped storageはcrypto providerとは別の責務です。

power lossを考慮したrecord形式、readback validation、active/inactive slot、provisioning boundaryを設計してください。

raw keyを通常applicationのconsole commandから受け付けないでください。

既存方式とsecurity boundaryについては[Security guide](security.md)を参照してください。

## 13. Storage SentinelとNPUを追加する

passive observationとCPU inferenceはtarget非依存sourceだけでも構成できますが、正しいmonotonic timingとdiagnostics snapshotが必要です。

まずCPU pathと公開corpusを使ってfeature schema、baseline、scoreを確認した後でNPU providerを追加します。

target NPUでは`mtfs_sentinel_npu_provider_ops_t`の次のoperationを実装します。

* `inspect`: runtime binaryを実行せず、ABI、region、alignment、hashなどを報告
* `install`: authentication/policy検証済みruntimeを指定memoryへ配置
* `infer`: timeout付きで1 windowを処理
* `close`: runtimeを停止してtarget resourceを解放
* `lock`/`unlock`: acceleratorへのアクセスを直列化
* `zeroize`: policyで指定された機密regionを消去

providerを登録するだけではapplication側のlifecycleは完結しません。

application側で、認証済みbundleのlifetime、copy/activation/external RAM region、cache、arena、timeout、およびopen/infer/closeの処理を組み合わせる必要があります。

`inspect`結果とbundle policyが一致しないruntimeをinstallしないでください。

bundleのbacking bufferと、それを参照するparsed viewは、NPUをcloseするまで内容を変更せず保持してください。

CPU/NPUの固定corpusで結果が一致すること、timeout/cleanup、stack/heap deltaを確認した後、target固有のoptimized Release profileでclassification qualityを評価します。

Debug buildやpolling/fallbackは機能確認には利用できますが、正式なquality評価には使用しません。

詳しくは[Storage Sentinel guide](storage-sentinel.md)と[target profile](architecture/target-profiles.md)を参照してください。

## 14. 段階的に検証する

最初からすべてのtestを一度に通そうとせず、問題が発生した場合にどの層で失敗したかを特定できる順序で進めます。

| 段階              | 実行内容                                                 | 次へ進む条件                                 |
| --------------- | ---------------------------------------------------- | -------------------------------------- |
| A. 共通build      | Host test、C/C++ public header compile、feature on/off | 共通APIとmacroの不一致がない                     |
| B. Target build | Debug/Release clean build、linker map確認               | warning/error、未解決symbol、意図しないheap依存がない |
| C. Block init   | initialize、status、geometry、read-only raw read        | 実mediaのstate/geometryと一致する             |
| D. FatFs        | read-only mount、`apps/simple`、`test-roundtrip 1`     | mountから逆順cleanupまでPASS                 |
| E. Repetition   | target既定のroundtrip、normal/stress profile             | timeout、counter、stack、resource leakがない |
| F. Lifecycle    | hotplug、write-protect、timeout、再initialize            | stale file/IRQ/stateを再利用しない            |
| G. DMA          | alignment違い、single/multi-block、cache、abort           | pollingとdataが一致し、late IRQを処理できる        |
| H. Optional     | RTC、crypto negative、sealed package、CPU/NPU corpus    | 有効化したfeatureのcontractを満たす              |

Host testの標準commandについては[Getting Started](getting-started.md#hostで試してみる)、target console commandについては[Application / console manual](applications.md#target-test-application)を参照してください。

benchmarkでは、raw read-only pathと、専用temporary fileを使用するFatFs pathだけを使用します。

format、raw write、trim、既存の利用者dataの上書きを自動testへ含めないでください。

問題が発生した場合は[Troubleshooting](troubleshooting.md)に従い、common/media/target diagnosticsを同じ時点で保存してください。

## 15. Portに含める文書

新しいportと一緒に、少なくとも次の情報を記録してください。

* 対応board/MCU、HAL/SDK/toolchain version
* pin、clock、bus width、電源、Card Detect、write-protect
* DMA可能RAM、cache line、linker section、IRQ priority
* config既定値、timeout、最大transfer/bounce size
* HAL/protocol errorから`mtfs_error_t`へのmapping
* context/registry/media/providerのlifecycleとshutdown順序
* diagnostics fieldとvalidity条件
* Debug/Release build手順とvalidation結果
* 非対応featureと既知の制約

board固有情報は[Board configuration](board-configuration.md)と同程度の粒度で記載し、generated projectだけを唯一の仕様として扱わないでください。

## Port completion checklist

以下は実装を始めるための手順ではなく、上記ガイドに沿って実装したportの最終確認に使用します。

対象外のoptional featureについては、未確認のままcheckを付けず、「対象外」であることとその理由を記録してください。

### Scopeとbuild

* [ ] 移植levelと対象外featureを文書化した
* [ ] transport portとboard/generated設定の責務を分離した
* [ ] 必要sourceと同じ`MTFS_*` macroをすべてのtranslation unitへ供給した
* [ ] Host testとC/C++ public header compileを通した
* [ ] feature enabled/disabledで必要な組み合わせをcompileした
* [ ] target Debug/Releaseをclean buildした

### Block device

* [ ] `mtfs_block_device_t`と、必要なlifetimeを持つdriver contextを用意した
* [ ] initialize、status、read、write、sync、geometryを実装した
* [ ] trim非対応の場合はcapabilityを有効にせず、`NOT_SUPPORTED`を返す
* [ ] LBA/count、0 count、buffer、HAL型変換、byte数overflowをtransfer開始前に検査した
* [ ] sector size、sector count、erase block sizeをgeometryで正確に返す
* [ ] INITIALIZED、MEDIA_PRESENT、WRITE_PROTECTEDを実際の状態に対応させた
* [ ] READ_ONLY、TRIM、DIAGNOSTICS capabilityを実装内容と一致させた
* [ ] HAL/protocol errorを安定した`mtfs_error_t`へmappingした
* [ ] callbackはcaller task contextで完了し、partial successを成功として扱わない
* [ ] I/O、registry、volume lifecycleの直列化範囲を定義した

### OS、media、time

* [ ] device context内または上位層でI/Oを直列化し、registry操作も同期した
* [ ] FatFsをreentrant構成にする場合は適切なmutex adapterを選択した
* [ ] removable media IRQではedgeだけを通知し、task contextでdebounceした
* [ ] remove/insert時にmedia generationを更新し、I/OのNO_MEDIAとSentinelのDISCONTINUITYへ反映した
* [ ] notification受付停止、IRQ停止、worker/context deinitのshutdown順序を確認した
* [ ] RTC有効時はlocal calendar provider、lock、UNSET marker、clearを実装した
* [ ] initialize/register/mountと逆順のcleanupを、すべての失敗箇所で確認した

### Diagnosticsとtiming

* [ ] common block diagnosticsをI/O開始前にattachした
* [ ] request、completion、failure、timeout、abort、transport counterを実装した
* [ ] API version、struct size、validity maskを定義した
* [ ] reset epochを更新し、cached stateは維持した
* [ ] counterはwraparoundさせず、最大値で飽和させた
* [ ] monotonic clock errorとtiming validityを区別した
* [ ] instrumentation自体が追加のmedia I/Oやconsole出力を発生させない

### DMA / cache / IRQ

* [ ] DMA可能RAM regionをlinker mapで確認した
* [ ] cache line alignmentと、bufferが使用するcache line全体の専有を保証した
* [ ] alignment要件を満たさないuser bufferにはaligned bounce bufferを使用した
* [ ] write前のclean、read前のclean/invalidate、read完了後のinvalidateを実装した
* [ ] IRQ callbackでhandleを照合し、event flagを使ってtaskへ完了を通知した
* [ ] absolute timeout、abort、late IRQ、spurious IRQ、deinitを処理した

### Sealed model（使用する場合）

* [ ] opaque handleだけを公開するcrypto providerを実装した
* [ ] key ID/version、authentication failure、resource不足を区別した
* [ ] authentication failure時にplaintextを公開せず、一時dataをzeroizeした
* [ ] hardware cryptoへのアクセスを必要な範囲で直列化した
* [ ] wrapped fleet-key storeとprovisioning boundaryを文書化した
* [ ] tag/ciphertext/key/policyのnegative testを通した

### Storage Sentinel / NPU（使用する場合）

* [ ] diagnostics/timingから同じfeature schemaを生成できることを確認した
* [ ] CPU reference corpusを先に通した
* [ ] inspect/install/infer/close/lock/unlock/zeroizeを実装した
* [ ] runtime memory region、alignment、hash、ABI policyを検証した
* [ ] timeout、close、zeroize、失敗時のunlockを確認した
* [ ] optimized Release profileでCPU/NPU numerical acceptanceを通した

### Target validation

* [ ] `apps/simple`、`test-roundtrip 1`、通常profile、stressの順に通した
* [ ] read-only raw testとtemporary-file FatFs benchmarkだけを使用した
* [ ] hotplug、write-protect、timeout、cleanup後の再initializeを確認した
* [ ] diagnostics resetとstack high-waterを確認した
* [ ] raw write、自動format、利用者dataの上書きをtestへ含めていない
* [ ] board設定、memory/IRQ条件、error mapping、制約を文書化した
