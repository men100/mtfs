# Troubleshooting

この文書では、症状から最初に確認すべき箇所を探すための早見表と、原因を安全に切り分けるための詳細な手順をまとめています。

実行中のbinaryによって、利用できるconsole commandは異なります。まず`help all`を実行し、そのbinaryで実際に利用できるcommandと構文を確認してください。

## 早見表

| 症状                                                                  | 確認箇所                                              | 最初の対処                                    |
| ------------------------------------------------------------------- | ------------------------------------------------- | ---------------------------------------- |
| [include file not found](#include-file-not-found)                   | IDE linked resource、include path、submodule        | repository相対pathとsubmoduleの状態を確認         |
| [symbol未定義 / sourceがbuildされない](#undefined-symbol--source-excluded)  | Debug/Releaseのsource exclusion                    | 必要なsourceを両configurationへ追加してclean build |
| [SD initialize失敗](#sd-initialize-failure)                           | card電源、Card Detect、geometry、transport diagnostics | `diag`を使ってmedia、common、targetの順に原因箇所を確認  |
| [RAでSDを認識しない](#ra-sd-not-detected)                                  | PMOD2 SCK/MISO/MOSI/CS、J1 pin 9 CD                | 配線、board設定、FSP生成結果を照合                    |
| [RA IRQ6が入らない](#ra-irq6-not-fired)                                  | P409 IRQ6、P000 `IRQ6-DS`                          | P000側のIRQを無効化してFSP code generation       |
| [`f_mount()`失敗](#f_mount-failure)                                   | initialize、registry、FAT12/16/32、sector size       | transportが正常であることを確認してからmount条件を確認       |
| [write protected](#write-protected)                                 | block status、card adapter lock                    | mediaのlockを解除。write-protectは無視しない        |
| [timestampが固定 / 0](#timestamp-fixed-or-zero)                        | `MTFS_FF_FS_NORTC`、provider、`rtc-status`          | 構成とRTC状態を確認してから`rtc-set`                 |
| [LFNだけ失敗](#lfn-failure)                                             | LFN macro、`ffunicode.c`、stack                     | macroとsourceをすべてのtranslation unitで統一     |
| [public struct size / link挙動が不一致](#public-abi-mismatch)             | translation unitごとの`MTFS_*` macro                 | defineの供給元を一か所にまとめてfull rebuild          |
| [task fault / 不規則な破損](#task-fault-or-corruption)                    | `stack-highwater`、guard、work area                 | fault発生時のstack marginとguard状態を保存         |
| [DMA timeout / data化け](#dma-timeout-or-corruption)                  | DMA可能RAM、32-byte alignment、cache                  | buffer配置とcache maintenanceの順序を確認         |
| [ST SDMMC initialize失敗](#st-sdmmc-initialize-failure)               | Cube `.ioc`、RIF CID/security、SDMMC2 clock         | generated設定とboard portの前提を照合             |
| [ST write後ready timeout](#st-ready-timeout-after-write)             | BUSYD0END IRQ、event flag、priority                 | ready関連counterとIRQ handlerを確認            |
| [wrapped fleet key not found](#wrapped-fleet-key-not-found)         | provisioner `info` / verify、key slot              | 専用provisionerでread-onlyの確認を実施            |
| [wrong fleet key / authentication failure](#authentication-failure) | package、key ID/version、provider                   | retryやbypassを行わず、packageとkeyの組み合わせを確認    |
| [target / accelerator mismatch](#model-policy-mismatch)             | `model-info`、manifest ID                          | targetに対応するpackageを選び直す                  |
| [`SENTINEL.MTF`がない](#sentinel-package-missing)                      | SD root、filename、公開artifactの範囲                    | RAはtest package、STは利用者環境で生成したpackageを配置  |
| [Sentinelがwarmup表示のまま](#sentinel-warmup-does-not-complete)          | valid window数、media generation、I/O activity       | baseline progressとwindowが除外された理由を確認      |
| [OOD / RULE表示](#sentinel-ood-or-rule)                               | state、source、saturation、hard error、discontinuity  | `diag`と判定metadataを同時に保存                  |
| [Debugとclassificationが異なる](#debug-classification-differs)           | build profile、transport、timing分布                  | quality評価はoptimized Releaseで再実施          |
| [flash後bootしない](#boot-failure-after-flash)                          | header、address、FSBL/Appliの組み合わせ                   | dry-run後、同じ組み合わせを`--target all`で再flash   |
| [whole-chip erase後にkeyが消えた](#fleet-key-lost-after-mass-erase)       | key slot A/B、provisioner                          | trusted environmentで再provision           |

## 最初に行うこと

1. commit ID、dirty状態、board、build configuration、IDE/compiler version、追加した`MTFS_*` macroを記録します。
2. 起動できる場合は`help all`の出力を保存し、そのbinaryで利用可能なdiagnostic commandを確認します。
3. 問題の操作を繰り返す前に、`diag`、`bench-info`、直前のPASS/FAIL行を保存します。`diag-reset`を使用する場合は、reset前のsnapshotを先に保存してください。
4. card、transport、電源、配線、再現手順、発生時刻を記録します。そのうえで、条件を一つずつ変更しながら原因を切り分けます。
5. clean build後も再現するか確認します。Debug/Release間で生成物やmacroを混在させないでください。

> **Dataとsecurityを保全してください。** mount失敗だけを理由にformatしないでください。authentication failureをI/O retryで回避しないでください。調査目的でwhole-chip erase/mass eraseを実行しないでください。

## Buildとsource integration

### Include file not found

1. [Source integration guide](integration.md#最小構成)で、利用するfeatureに必要なinclude rootとsourceを確認します。
2. IDEのlinked resourceとinclude pathが、特定のPCにしか存在しないabsolute pathではなく、repository相対pathになっていることを確認します。
3. `git submodule status --recursive`で未取得のsubmoduleがないことを確認し、必要であれば`git submodule update --init --recursive`を実行します。
4. filesystem上のfile名とinclude文の大文字/小文字が一致していることを確認します。Windowsでは通る構成でも、case-sensitiveなCIでは失敗する場合があります。
5. IDEのindexだけでなく、実際のcompiler command lineにも期待するinclude pathが含まれていることを確認します。

pathを修正した後は、IDEのindex更新だけで済ませず、対象configurationをclean buildしてください。

vendor生成fileを直接編集してpathの問題を回避しないでください。

### Undefined symbol / source excluded

headerが見つかっていても、対応するimplementationがlink対象に含まれていなければsymbol未定義になります。

1. [feature別source一覧](integration.md#feature別source)で必要な`.c`/`.cpp`を確認します。
2. IDEのsource exclusionをDebugとReleaseの両方で確認します。
3. CとC++で同じpublic headerを使用する場合は、宣言側のlanguage linkageと、実際にbuildされたobjectのsymbolを確認します。
4. 古いobjectや別configurationのlibraryが混在していないことを確認するため、full clean後に再buildします。

未定義symbolを1つずつ追加して対処するのではなく、そのfeatureに必要なsource一式と依存関係をまとめて確認してください。

### Public ABI mismatch

一部の`MTFS_*` macroは、public structのmember、FatFs型、source内で有効になるfeatureを変更します。

translation unitごとに設定値が異なると、compile自体は成功しても、struct size、field offset、linkされる実装が一致しない場合があります。

1. compiler command lineとgenerated makefileを確認し、同じmacroが重複定義されていないこと、configurationごとに意図せず上書きされていないことを確認します。
2. project rootの一か所から、C/C++を含むすべてのtranslation unitへ同じdefineを供給します。
3. library、application、testを含めてfull cleanし、すべてを同じ設定で再buildします。
4. public headerのcompile testとstatic assertionを再実行します。

既定値と各macroの意味については[Configuration reference](configuration.md)を参照してください。

ABI不一致を解消するために、production structやpublic APIをその場しのぎで変更しないでください。

## SD cardとfilesystem

### SD initialize failure

initializeとmountは別の処理です。まず、block deviceがcardを認識し、有効なgeometryを取得できるところまでを確認します。

1. cardが正しく装着されていること、3.3 V電源、Card Detect、write-protect状態を確認します。
2. `diag`のmedia snapshotでpresent/state/generationを確認します。absentの場合はfilesystemではなく、Card Detectまたは物理接続から調査します。
3. common snapshotの`last_operation`/`last_error`と、initializeのsuccess/failure数を確認します。
4. target snapshotのinitialization stage、timeout、driver/kernel errorを確認します。
5. 必要な記録を保存した後、既知の正常なcardを使って同じ手順を試します。

`NO_MEDIA`はmediaが存在しない状態です。

`NOT_READY`は、挿入直後、debounce中、未初期化など、mediaは存在していてもI/Oを開始できる状態になっていないことを示します。

両者を同じ「mount失敗」として扱わないでください。

正常時には、mediaがpresentかつstableな状態になり、initializeが成功し、有効なsector size/countが取得されます。

この段階で失敗している場合、cardをformatしてもtransportやCard Detectの問題は解消しません。

diagnostics fieldの読み方は[Storage operations](storage-operations.md#diagnostics)、board固有の条件については[Board configuration](board-configuration.md)を参照してください。

### RA SD not detected

EK-RA8P1ではDigilent Pmod MicroSD Revision AをPMOD2へ接続し、3.3 Vで使用します。

1. SCK=P601、MISO=P602、MOSI=P603、CS=P604、CD=P409であることを[配線表](board-configuration.md#ek-ra8p1)と照合します。
2. CSのactive levelとCard Detectのactive-low設定を確認します。
3. FSP `configuration.xml`のSCI0 SPI pinmux、P409 pinmux、external IRQ6 callbackを確認します。
4. FSP code generationを実行し、生成内容をbuildへ反映します。

`bsp_pin_cfg.h`を直接編集すると次回のcode generationで変更が失われるため、FSP Pins/Stacksから設定してください。

### RA IRQ6 not fired

P409をexternal IRQ6へ割り当てる場合は、P000の`IRQ6-DS`を無効にし、P000を通常のGPIO inputとして使用します。

P000とP409の両方を同時にIRQ6 sourceへ割り当てると競合します。

1. FSP PinsでP409だけがIRQ6 sourceになっていることを確認します。
2. edge、active level、callback、IRQをenableするタイミングを確認します。
3. code generationを行い、clean buildします。
4. cardの抜き差し前後で、media diagnosticsのIRQ通知、debounce開始/再確認、insert/remove eventが増加することを確認します。

IRQ通知はtask contextでdebounceされます。

ISRへ入ることだけでなく、安定したmedia stateが確定し、media generationが更新されるところまで確認してください。

### `f_mount()` failure

次の順序で、どの層で失敗しているかを確認します。

1. block deviceのinitializeが成功し、geometryが有効であることを確認します。
2. deviceがregistryへ正しく登録され、slotとFatFsのphysical drive番号が一致していることを確認します。
3. `f_mount()`へ渡しているlogical driveと`MTFS_FF_VOLUMES`を確認します。
4. mediaが対応するFAT12/16/32でformatされていること、sector sizeがtarget/FatFs構成と一致していることを確認します。
5. hotplug後であれば、以前のfile objectを再利用せず、initialize、register、mountをやり直します。

transportは正常で、mountだけが失敗する場合は、FatFs resultとcommon diagnosticsを一緒に記録してください。

mount失敗時に自動でformatすることはありません。既存dataを保全し、別環境でfilesystemを確認したうえで、明示的なmaintenance/factory workflowとしてformatが必要か判断してください。

block deviceとFatFsの関係は[Storage operations](storage-operations.md#block-deviceとfatfs)、関連するmacroは[Configuration reference](configuration.md#block-registryとvolume)を参照してください。

### Write protected

1. block statusを確認します。adapterに物理的なlock switchがある場合は、その位置も確認します。
2. write-protectは書き込み開始前に検出し、open済みfileや上位処理へerrorとして返します。
3. lockを解除した後はmedia stateを再確認し、必要に応じてinitialize/register/mountをやり直します。

driverを変更してwrite-protectを無視したり、失敗したwriteを成功として扱ったりしないでください。

### Timestamp fixed or zero

固定日時になる場合と`0`になる場合では、原因が異なります。

* `MTFS_FF_FS_NORTC=1`では、仕様どおり`2025-01-01 00:00:00`の固定日時を使用します。
* `MTFS_FF_FS_NORTC=0`で`0`になる場合は、provider未登録、RTC未設定/error、またはFatFsで表現できない日時が原因です。

RTCを使用する場合は、必要なcore/FatFs/target RTC sourceをbuildへ追加し、起動時にproviderをregisterします。

consoleを利用できるtargetでは、`rtc-status`で`UNSET`/errorを確認し、必要な場合だけ`rtc-set YYYY-MM-DD hh:mm:ss`を実行してread-backを確認します。

timezone、DST、時刻同期はapplication側の責務です。詳しくは[RTCとtimestamp](storage-operations.md#rtcとtimestamp)を参照してください。

### LFN failure

8.3 filenameでは成功し、LFNだけが失敗する場合は、次を確認します。

1. `MTFS_FF_ENABLE_LFN`、`MTFS_FF_MAX_LFN`、`MTFS_FF_LFN_UNICODE`、code pageが、すべてのtranslation unitで一致していることを確認します。
2. LFN有効時は`src/fatfs/ffunicode.c`をbuildへ追加します。
3. microT-FSが対応する`FF_USE_LFN=2`ではcaller stack上にwork bufferを確保するため、`stack-highwater`で十分なstack marginがあることを確認します。
4. 正式に対応している文字範囲のfilenameでも再現するか確認します。現行の正式対応範囲はASCII subsetです。

LFN bufferだけでも、caller stack上で少なくとも`(MTFS_FF_MAX_LFN + 1) * 2` byteを使用します。

heap modeや共有static bufferへ変更して症状だけを回避するのではなく、[FilenameとLFN](configuration.md#filenameとlfn-long-file-name)に記載した構成へ合わせてください。

## Memory、DMA、target固有transport

### Task fault or corruption

1. fault直後のregister/fault statusを保存します。可能であれば`stack-highwater`の出力も保存してください。
2. coordinator/workerごとのused/free/margin、guard、overall statusを確認します。
3. LFN、crypto、sealed model、Sentinel inferenceのwork areaを同じtask stackへ追加していないか確認します。
4. stackを増やして症状が消えるかだけでなく、どの処理によってhigh-waterが増加したかを再測定します。

guard failureが発生した場合は、すでにstack境界を越えている可能性があります。

その実行で得られた後続結果は、正常なvalidation結果として扱わないでください。

static arena、task stack、heap、DMA bufferは、それぞれ個別に使用量を確認します。

測定方法については[Performance / resource reference](performance.md#resource-usage)を参照してください。

### DMA timeout or corruption

1. DMA/IDMA bufferとdescriptorが、そのengineからアクセス可能なRAMへ配置されていることをlinker mapで確認します。
2. buffer addressとlengthがcache lineの要件を満たしていることを確認します。現行target diagnosticsでは32-byte alignmentも確認します。
3. CPUからdeviceへ渡す前のcleanと、deviceからCPUへ戻った後のinvalidateについて、実行順序と対象範囲を確認します。
4. requested/completed sector数、completion/error callback、timeout、abort counterを比較します。
5. polling fallbackと比較する場合はdiagnostics目的に限定し、正式なIDMA＋IRQ profileと同等の評価結果として扱わないでください。

cache maintenanceを広い範囲へ追加して症状だけを抑える前に、bufferの所有権が切り替わる箇所とbounce bufferの実際の範囲を確認してください。

### ST SDMMC initialize failure

1. on-board socketがSDMMC2、4-bit bus、Card DetectがPN12/EXTI12であることを[board設定](board-configuration.md#stm32n6570-dk)と照合します。
2. Cube `.ioc`、generated MSP、clock source/divider、GPIO alternate function、bus widthを確認します。
3. SDMMC2とDMA/IDMAが使用するresourceについて、RIF CID/security属性がapplicationの実行domainと一致していることを確認します。
4. Card Detectがstable presentになったことを確認してからinitializeしていることを確認します。
5. target diagnosticsから、HAL initialized、initialization stage、last HAL/kernel errorを保存します。

generated `main.c`や`stm32n6xx_hal_msp.c`だけを直接修正せず、`.ioc`とboard portそれぞれの責務に沿って設定を修正してください。

### ST ready timeout after write

STのIDMA＋IRQ pathでは、data transferの完了とcardがready状態へ戻ることは別の完了条件です。

1. SDMMC2 IRQ handlerがHAL handlerへ接続され、BUSYD0END interruptがenableされていることを確認します。
2. SDMMC2 IRQ priorityが、待機taskを起床させるkernel APIの制約を満たしていることを確認します。
3. event flagをwait前に過剰にclearしていないこと、過去のeventを新しい成功通知として再利用していないことを確認します。
4. `diag`で`ready_sequences`、`ready_event_waits`、`busyd0end_irqs`、`ready_event_wakeups`を比較します。正常なevent pathでは対応するcounterが進み、`ready_wait_timeouts`は増加しません。
5. timeout発生時は、card state、HAL error、abort、hybrid fallbackも同じsnapshotで確認します。

timeout値を単に延ばす前に、IRQ自体が発生していないのか、IRQは発生しているがwaiterへ届いていないのか、cardがready状態へ戻っていないのかを切り分けてください。

## Sealed model、fleet key、package

### Wrapped fleet key not found

通常applicationからraw keyを登録するのではなく、対象board向けの`apps/key-provision`を使用します。

1. trusted local UART sessionで専用provisionerを起動します。
2. `help provisioning`と`info`でprovider、保存先、active slot、key ID/versionを確認します。
3. RAでは`verify-ospi`、STでは`verify-nor`を実行し、read-onlyのcrypto validation結果を確認します。
4. keyが未登録の場合に限り、管理された32-byte binary key fileを`provision-xmodem`で初回登録します。

consoleへのpaste、hex/Base64 text、removable SD経由でraw keyを登録しないでください。

詳しい手順と現在のkey rotation制約については[`apps/key-provision`](applications.md#appskey-provision)を参照してください。

### Authentication failure

authentication failureは、wrong key、package破損、metadata/AADの不一致、key ID/version不一致などを含むsecurity errorです。

1. original packageを変更せず保存し、Hostで`mtfs-verify`を使ってformat、AEAD、期待するtarget/accelerator/format policyを検証します。
2. provisionerのread-only verifyで、device側のkey ID/versionとcrypto validationを確認します。
3. package作成時に使用したfleet keyと、deviceへprovisionしたkeyの管理記録を照合します。
4. 現行Host toolがpackageへ記録するkey ID/versionは`1/1`固定です。`update-xmodem`後のversion 2以降とは一致しないことを確認してください。

認証前のplaintextを利用したり、tag検証を無効にしたり、authentication failureを別のerrorとして扱って処理を続行したりしないでください。

Host toolとkeyの扱いについては[Security guide](security.md#host-tools)を参照してください。

### Model policy mismatch

packageが正しく認証されていても、target、transport、accelerator、model format、profile ID、required RAMが実行環境と一致しなければ拒否されます。

`model-info`、package manifest、生成時に使用したcommand、target側の期待値を照合し、正しいtarget向けpackageを選択するか再生成してください。

policy checkを無効化して、別target向けartifactを流用しないでください。

RA/Ethos-U55向けpackageとST/Neural-ART向けpackageには互換性がありません。

### Sentinel package missing

`SENTINEL.MTF`をSD cardのroot directoryへ正確な大文字のfile名で配置し、targetから`0:/SENTINEL.MTF`として参照できることを確認します。

RAでは、公開test key用の[`SENTINEL.MTF`](../artifacts/storage_sentinel/models/ek_ra8p1/SENTINEL.MTF)を利用できます。これはdemo/test専用であり、production用途には使用できません。

STではlicense上の公開範囲により、Neural-ART generated runtimeを含む`SENTINEL.MTF`をrepositoryで配布していません。

公開recipe、canonical TFLite、audit記録に加え、利用者が正規に取得したST toolchainとtarget memory layout向けprofileを使用して、利用者環境で生成してください。

単なる配布漏れではありません。

作成方法は[STM32N6570-DK用`SENTINEL.MTF`の生成](storage-sentinel.md#stm32n6570-dk用sentinelmtfの生成)、公開範囲の理由は[RAとSTで公開artifactが異なる理由](storage-sentinel.md#raとstで公開artifactが異なる理由)を参照してください。

## Storage Sentinel

### Sentinel warmup does not complete

公開RA/ST profileでは、sessionごとに32個の有効なwindowを使ってbaselineを作成します。

単に32回windowが表示されればwarmupが完了するわけではありません。

1. `baseline_progress/required`が進んでいることを確認します。
2. 通常I/Oが継続しており、有効なwindowが生成されていることを確認します。
3. hard error、media discontinuity、invalid snapshot、pseudo injectionなどによってwindowが除外されていないか確認します。
4. `diag`でmedia generationが変化していないこと、remove/insert/error eventが増加していないことを確認します。
5. card再挿入後はinitialize/register/mountをやり直し、新しいgenerationでwarmupが完了するまで待ちます。

以前のcard/sessionで確立したbaselineを引き継いだり、異常注入中のwindowをwarmupへ含めたりしないでください。

処理の意味については[Baseline-relative preprocessing v2](storage-sentinel.md#baseline-relative-preprocessing-v2)を参照してください。

### Sentinel OOD or RULE

`source=RULE`は、「AIが異常と判定した」ことを意味しません。

AIによる判定へ進めない状態をdeterministicに分類した結果です。`state`と`source`を必ず組み合わせて確認してください。

* `WARMUP`: baselineを収集中です。progressとwindowが除外された理由を確認します。
* `OOD`: baseline-relative変換が許容範囲を超えています。saturation count/maskとraw/baseline値を確認します。
* `BLOCK_ERROR`: storage I/O errorを検出しています。common/target diagnosticsのerrorとtimeoutを確認します。
* `DISCONTINUITY`: snapshotまたはmedia sessionの連続性が失われています。新しいsessionとしてwarmupをやり直します。
* `NO_MEDIA` / `NOT_READY`: media不在、または再初期化待ちの状態です。inferenceは開始されません。

`OOD`を故障確定や`ANOMALY`と同じ意味で扱わないでください。

また、`ANOMALY`だけを根拠にcard交換、format、data消去を実行せず、電源、配線、clock、workload、hotplug履歴、diagnosticsをあわせて確認してください。

出力の詳しい読み方については[判定結果の読み方](storage-sentinel.md#判定結果の読み方)を参照してください。

### Debug classification differs

Debug buildでは、optimization、instruction timing、log量、stack/memory behaviorがRelease buildと異なります。

また、STのpolling fallbackは、正式なIDMA＋IRQ profileとはcompletion behaviorが異なります。

Debug buildやpollingは機能確認や原因切り分けには利用できますが、classification qualityの正式な評価には使用しません。

targetに対応するoptimized Release reference profileで、同じpackage、card/session、workload条件を使用して再評価してください。

reference条件と既知の結果については[Performance](performance.md#storage-sentinel-reference)を参照してください。

## STM32 image deploymentと復旧

### Boot failure after flash

1. FSBLとAppliが同じbuild/commitから生成された組み合わせであることを確認します。
2. no-key trusted-header生成時のinput、output、`STM2` magic、payload検証結果を確認します。
3. flash前に`flash_binary.py`を`--target all --dry-run`で実行し、FSBL=`0x70000000`、Appli=`0x70100000`、external loader、probeの設定を確認します。
4. 電源とST-LINKが安定していることを確認し、hash確認済みの同じFSBL/Appliを`--target all --confirm-flash`で再度書き込みます。
5. FSBLとAppliの両方についてprogram/verifyが成功したことを確認します。片方だけ成功した状態は書き込み完了として扱いません。

toolは、部分的に更新されたexternal flashを自動でrollbackしません。

address、key slotとの重複、復旧手順については[STM32 deployment tools](stm32-deployment.md)を参照してください。

### Fleet key lost after mass erase

STM32N6570-DKでは、fleet key slot A/Bを`0x77ffe000`/`0x77fff000`に配置しています。

whole-chip erase/mass eraseを実行するとこれらのkey slotも消去されるため、通常のfirmware再書き込みや障害調査には使用しないでください。

すでに消去してしまった場合は、trusted environmentで`apps/key-provision`を起動し、管理している元のfleet keyを初回登録手順に従って再provisionします。

その後、`verify-nor`でkey ID/versionとcrypto validationを確認し、対応するpackageを再度検証します。

keyのbackupが存在しない場合、既存packageを認証・復号できなくなる可能性があります。

復旧のためにpackage側のauthenticationを無効化しないでください。
