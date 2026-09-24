# Configuration reference

設定の既定値は[`src/mtfs_config.h`](../src/mtfs_config.h)で定義しています。必要に応じてbuild systemから上書きできますが、public headerをincludeするすべてのtranslation unitで同じ値を使用してください。

設定値が一致していない場合、public structのlayoutやFatFs ABIが変わる可能性があります。各設定に必要なsourceについては[Source integration guide](integration.md)を参照してください。

## 一覧

| macro                                    |                             既定値 |
| ---------------------------------------- | ------------------------------: |
| `MTFS_ENABLE_DIAGNOSTICS`                |                             `1` |
| `MTFS_ENABLE_STORAGE_SENTINEL`           |                             `0` |
| `MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE` |                             `0` |
| `MTFS_ENABLE_SEALED_MODEL`               |                             `0` |
| `MTFS_BLOCK_REGISTRY_SIZE`               |                             `4` |
| `MTFS_FF_VOLUMES`                        |                             `1` |
| `MTFS_FF_ENABLE_LFN`                     |                             `0` |
| `MTFS_FF_MAX_LFN`                        |                            `64` |
| `MTFS_FF_LFN_UNICODE`                    |                             `0` |
| `MTFS_FF_CODE_PAGE`                      |                           `437` |
| `MTFS_FF_FS_REENTRANT`                   |                             `0` |
| `MTFS_FF_FS_TIMEOUT`                     |                          `1000` |
| `MTFS_FATFS_MUTEX_ADAPTER`               | `MTFS_FATFS_MUTEX_ADAPTER_NONE` |
| `MTFS_FF_FS_NORTC`                       |                             `1` |
| `MTFS_FF_USE_MKFS`                       |                             `0` |
| `MTFS_FF_FS_READONLY`                    |                             `0` |
| `MTFS_STM32_SD_USE_IDMA`                 |                             `1` |

`MTFS_STM32_SD_USE_IDMA`のみSTM32N6570-DKのboard headerで定義されます。それ以外の設定はportableな`mtfs_config.h`で定義されています。

## Feature switch

### `MTFS_ENABLE_DIAGNOSTICS`

`0`または`1`を指定します。

`1`では、block operation、media lifecycle、target portに関するversion付きsnapshotと飽和counterを有効にします。block deviceやport contextのpublic layoutにもdiagnostics stateが含まれるため、library sourceとapplicationで異なる値を使用してはいけません。

`0`では、diagnosticsの取得/reset APIは`MTFS_ERROR_NOT_SUPPORTED`を返します。

`mtfs_block_diagnostics.c`はdisabled時のpublic API stubも提供するため、設定値にかかわらずbuildへ含めることを推奨します。

`MTFS_ENABLE_STORAGE_SENTINEL=1`ではdiagnosticsを観測元として使用するため、`MTFS_ENABLE_DIAGNOSTICS=0`には設定できません。

動作確認では、通常I/Oの実行後にsnapshotのAPI version、size、counter、last errorを確認します。また、`diag-reset`によってI/Oやmedia状態が変化せず、counterとreset epochだけが更新されることを確認してください。

### `MTFS_ENABLE_STORAGE_SENTINEL`

`0`または`1`を指定します。

`1`では、block deviceのdownstreamへpassive observerを接続し、既存I/Oから取得したtiming/error counterをもとにfeatureを生成します。Storage Sentinel自身が観測のために追加のmedia I/Oを発行することはありません。

有効にすると、observer、feature frame、baseline state、diagnostics lockのためのcode/RAMが追加で必要になります。

次の依存関係はcompile時に検査されます。

```text
MTFS_ENABLE_STORAGE_SENTINEL=1
  requires MTFS_ENABLE_DIAGNOSTICS=1
```

このswitchだけでは、model bundleのparse、CPU inference、NPU runtimeは有効になりません。必要なsourceとlifecycleについては[Source integration guide](integration.md#storage-sentinel)、判定経路については[Storage Sentinel guide](storage-sentinel.md)を参照してください。

### `MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE`

`0`または`1`を指定します。

`1`では、heapを使用しないfixed-point bundle parser、memory planner、normalization、CPU inference、score判定を有効にします。

次の依存関係があります。

```text
MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE=1
  requires MTFS_ENABLE_STORAGE_SENTINEL=1
```

このswitchを有効にしても、NPU providerやsealed storageが自動的に選択されるわけではありません。

CPU inferenceのみを使用する場合はtarget NPU providerは不要です。NPUを使用する場合はgeneric NPU providerとtarget runtimeを追加します。認証済みbundleをSD cardから読み込む場合は、`MTFS_ENABLE_SEALED_MODEL=1`とsealed adapterも追加してください。

bundle、runtime region、arenaのsize/alignmentはtarget profileと一致させる必要があります。また、CPU/NPUの結果一致、timeout処理、close後のzeroizeまで確認してください。

Debug buildやfallback pathで得られたclassification qualityは、正式なreference profileによる評価結果としては扱いません。

### `MTFS_ENABLE_SEALED_MODEL`

`0`または`1`を指定します。

`1`では、sealed format/reader、model store、secure zeroizeのAPIを有効にします。使用する場合は、target側のhardware-backed crypto providerとfleet key storeを接続する必要があります。

FatFsだけを使用する構成へcrypto依存を持ち込まないため、既定値は`0`です。

providerはopaque key handleだけを公開し、fleet keyやmodel keyのplaintextをprovider外部へ渡してはいけません。

実装時には、authentication失敗時にplaintextを公開しないこと、途中で失敗した場合にdestination/temporary bufferをzeroizeすること、すべてのhandleをcloseすることを確認してください。

必要なsourceとRA/ST providerについては[Source integration guide](integration.md#sealed-model)、運用上のsecurity boundaryについては[Sealed Model / Security guide](security.md)を参照してください。

## Block registryとvolume

### `MTFS_BLOCK_REGISTRY_SIZE`

block registryが保持できる`mtfs_block_device_t *`の固定slot数です。`0`を指定するとcompile errorになります。

有効なphysical drive番号は`0`から`MTFS_BLOCK_REGISTRY_SIZE - 1`までです。registryはdevice本体の所有権を取得しません。

値を増やすと、staticなpointer tableのサイズも増加します。

実際に使用する最大physical drive番号に合わせ、必要以上の未使用slotを確保しない値を指定してください。また、`MTFS_FF_VOLUMES`以上である必要があります。

### `MTFS_FF_VOLUMES`

FatFsで使用できるlogical volume数です。指定できる範囲は`1..10`です。

次の関係はcompile時に検査されます。

```text
1 <= MTFS_FF_VOLUMES <= 10
MTFS_FF_VOLUMES <= MTFS_BLOCK_REGISTRY_SIZE
```

現行構成では`FF_MULTI_PARTITION=0`、`FF_STR_VOLUME_ID=0`のため、logical drive `0:`、`1:`などは同じ番号のphysical driveへ対応します。

volume数を増やす場合は、各deviceのregister/unregister、各`FATFS` objectのlifetime、reentrant構成で必要となるmutex object数も確認してください。

## FilenameとLFN (Long File Name)

### `MTFS_FF_ENABLE_LFN`

`0`または`1`を指定します。

`1`はFatFsの`FF_USE_LFN=2`に対応し、API callごとにcaller stack上へLFN work bufferを確保します。heapを使用する構成やstatic bufferを共有する構成は、microT-FSでは正式対応していません。

有効にする場合は`src/fatfs/ffunicode.c`をbuildへ追加し、すべてのtaskについて最悪時のstack marginを再測定してください。

この構成では`src/fatfs/ffsystem.c`は不要です。

LFNを有効にしても、正式に対応する文字範囲は後述するASCII subsetのままです。

### `MTFS_FF_MAX_LFN`

LFNで扱う最大UTF-16 code unit数です。

`MTFS_FF_ENABLE_LFN=1`の場合に限り`12..255`の範囲を指定でき、範囲外の値はcompile errorになります。LFNが無効な場合、この値はFatFsの動作に影響しません。

FatFsのLFN work bufferは、caller stack上で少なくとも`(MTFS_FF_MAX_LFN + 1) * 2` byteを使用します。

値を増やした場合は、最大長付近のfile create/lookup testとstack high-waterを再確認してください。

### `MTFS_FF_LFN_UNICODE`

現行のmicroT-FSでは`0`のみ指定できます。非ゼロ値を指定すると、LFNを使用しない構成であってもcompile errorになります。

そのため、APIの`TCHAR`はANSI/OEM `char`として扱われ、UTF-8、UTF-16、UTF-32のfilename APIは利用できません。

これは、media上にUnicode entryが存在できないという意味ではありません。microT-FSの公開APIでは、そのようなfilenameの読み書きや相互運用性を正式には保証しないという意味です。

### `MTFS_FF_CODE_PAGE`

既定値の`437`は、ASCII互換のU.S. OEM code pageです。

bundled FatFsがcompile時に受け付ける値は、`0`、`437`、`720`、`737`、`771`、`775`、`850`、`852`、`855`、`857`、`860`、`861`、`862`、`863`、`864`、`865`、`866`、`869`、`932`、`936`、`949`、`950`です。

ただし、microT-FSでqualificationを行っているprofileは`437`です。正式に対応するfilenameの文字は、ASCII英数字、space、`-`、`_`、`.`です。

8.3形式を標準とし、LFNでも正式対応する文字範囲は同じです。CP437拡張文字、日本語、Shift_JIS、Unicode、UTF-8 filename APIは正式対応外です。

code pageを変更する場合は、利用者側で変換table、ROM使用量の増加、既存mediaとの互換性を評価してください。

## Reentrancyとmutex

### `MTFS_FF_FS_REENTRANT`

`0`または`1`を指定します。

`1`では、同じvolumeに対するFatFsのfile/directory accessをmutexで直列化します。異なるvolumeへのaccessは、この設定にかかわらず独立しています。

一方、`f_mount()`、`f_mkfs()`、`f_fdisk()`などのvolume control operationは、この設定を有効にしてもreentrantにはなりません。これらのlifecycle操作はapplication側で直列化してください。

有効にする場合は、`MTFS_FATFS_MUTEX_ADAPTER`にPOSIXまたはmicroT-Kernelを指定し、対応するsourceをbuildへ追加します。`NONE`との組み合わせはcompile errorになります。

concurrent testに加えて、各taskのcleanup、mutexのcreate/delete failureも確認してください。

### `MTFS_FF_FS_TIMEOUT`

reentrant構成でmutexを取得するときのtimeoutです。

負の値を指定するとcompile errorになります。`MTFS_FF_FS_REENTRANT=0`の場合、この設定は使用されません。

既定値`1000`は、現行portでは1秒に相当します。POSIX adapterでは値をmillisecondへ変換して`pthread_mutex_timedlock()`へ渡し、microT-Kernel adapterでは`TMO`として`tk_loc_mtx()`へ渡します。

target側のkernel tick/timeout contractと一致していることを確認してください。

timeoutが発生するとFatFs operationを開始できないため、上位application側でretry、error報告、cleanupの方針を決める必要があります。

### `MTFS_FATFS_MUTEX_ADAPTER`

次の3つの値だけを指定できます。

| 値                                       | 用途                   | 追加source                                 |
| --------------------------------------- | -------------------- | ---------------------------------------- |
| `MTFS_FATFS_MUTEX_ADAPTER_NONE`         | non-reentrant構成      | なし                                       |
| `MTFS_FATFS_MUTEX_ADAPTER_POSIX`        | Linux / WSL2 Host    | `src/os/host/mtfs_fatfs_mutex_posix.c`   |
| `MTFS_FATFS_MUTEX_ADAPTER_MICROTKERNEL` | microT-Kernel target | `src/os/microtkernel/mtfs_fatfs_mutex.c` |

上記以外の値を指定するとcompile errorになります。

adapterは最大`MTFS_FF_VOLUMES + 1`個のmutex slotをstaticに保持します。FatFsを使用するすべてのtranslation unitで、同じadapter値を使用してください。

## Timestamp

### `MTFS_FF_FS_NORTC`

`0`または`1`を指定します。

既定値の`1`ではRTCへ依存せず、FatFsが書き込むtimestampにはlocal timeの`2025-01-01 00:00:00`が固定値として使用されます。

`0`では、`src/core/mtfs_time.c`、`src/fatfs/mtfs_fattime.c`、target RTC providerをbuildへ追加し、起動時にproviderをregisterします。

`get_fattime()`は、provider statusが`VALID`で、かつ日時がFatFsで表現可能な範囲にある場合だけpacked timestampを返します。RTCが未設定、異常状態、または日時が範囲外の場合は`0`を返します。

timezone/DST変換は行わず、providerから取得したlocal timeをそのまま使用します。

read-only構成ではFatFsがtimestampを書き込まないため、この設定はfileの更新時刻には影響しません。

RTCの設定方法、marker、異常時の扱いについては[Storage operations](storage-operations.md#rtcとtimestamp)を参照してください。

## Writeとformat

### `MTFS_FF_USE_MKFS`

`0`または`1`を指定します。

`1`は`f_mkfs()`をcompile対象に含める設定であり、mount失敗時に自動的にformatする機能を有効にするものではありません。

`MTFS_FF_FS_READONLY=1`の場合、`f_mkfs()`はbuildから除外されます。

formatは既存dataを失う可能性があるため、productionでは既定値の`0`を維持し、明示的なmaintenance/factory workflowでのみ有効にしてください。

通常applicationではmount failureを報告し、必要に応じて別の環境で準備したmediaを使用します。

### `MTFS_FF_FS_READONLY`

`0`または`1`を指定します。

`1`では、`f_write()`、`f_sync()`、`f_unlink()`、`f_mkdir()`、`f_rename()`、`f_truncate()`、`f_getfree()`などのFatFs write/maintenance APIがbuildから除外されます。

これはFatFs APIに対するcompile-time設定であり、underlying block deviceの`MTFS_BLOCK_CAPABILITY_READ_ONLY`とは別の仕組みです。

書き込みを確実に防止する必要がある場合は、FatFsのread-only設定だけでなく、block device capabilityやhardware write protectも適切に設定してください。また、block APIを直接呼び出す経路についても確認が必要です。

## STM32N6570-DK target設定

### `MTFS_STM32_SD_USE_IDMA`

portableな`mtfs_config.h`ではなく、`src/ports/stm32_cube/boards/stm32n6570_dk/mtfs_stm32n6570_dk_platform.h`で定義されるboard固有の設定です。

`0`または`1`のみ指定できます。

`1`ではSDMMC2のIDMA＋IRQ pathを使用します。context内の4 KiB bounce bufferは32-byte alignmentを持ち、DMAの前後でdata cacheのclean/invalidateを行います。

この構成では、適切なIRQ/RIF設定に加え、DMAからアクセス可能なmemory領域への配置とcache設定が必要です。

`0`ではpolling fallbackを使用し、IDMA completion IRQは使用しません。

pollingは機能確認やdiagnosticsには利用できますが、STM32N6570-DKのperformanceおよびStorage Sentinel classification qualityを評価する正式なreference profileはRelease build＋IDMA＋IRQです。

target固有のpin、Card Detect、IRQ設定については[Board configuration](board-configuration.md#stm32n6570-dk)、評価条件については[target profile](architecture/target-profiles.md#stm32n6570-dk)を参照してください。
