# Application / console manual

## 共通console

利用可能なcommandは、用途ごとにhelpから確認できます。

```text
help
help <group>
help all
log-level
log-level off|error|info|debug
````

runtimeのlog levelはIDEのDebug/Release buildとは独立しており、reset後は`info`へ戻ります。

通常の詳細logを抑止した場合でも、final result、security error、`ACTION REQUIRED`、dataset用CSVは出力されます。

## `apps/simple`

FatFsだけを使用する最小構成のsampleで、最初の移植確認に利用します。sealed modelとStorage Sentinelは無効です。

起動すると、mount、`HELLO.TXT`へのwrite、close、read/verify、delete、unmountを順に実行し、最後に`[simple] overall=PASS`を表示します。

block deviceのinitialize/registerとtarget固有のlifecycleは、各boardの`mtfs_target_main.c`が担当します。

## Target test application

[`tests/targets/ek_ra8p1`](../tests/targets/ek_ra8p1)と[`tests/targets/stm32n6570_dk`](../tests/targets/stm32n6570_dk)は、通常利用向けのsampleではなく、共通API contractとtarget portの動作を検証するためのtest runnerです。

### Console構文

RA8P1版とSTM32N6570-DK版で共通の構文です。角括弧内は省略可能な引数を表し、角括弧自体は入力しません。

```console
help
help <group>
help all
log-level [off|error|info|debug]

rtc-get
rtc-status
rtc-set YYYY-MM-DD hh:mm:ss
rtc-clear

test-roundtrip [rounds]
test-fatfs-time
test-hotplug

model-info
model-load
model-hotplug

bench-info
bench-smoke
bench-normal

diag
diag-help
stack-highwater

test-diagnostics-reset
diag-reset
crypto-info
crypto-consistency
crypto-negative
crypto-package-test
model-negative
```

`<group>`には`general`、`rtc`、`filesystem`、`media`、`model`、`benchmark`、`diagnostics`、`developer`を指定できます。`help all`を実行すると、そのbinaryに組み込まれているすべてのcommandを確認できます。

### 引数と動作

| command                                | 引数・既定値                                                        | 動作                                                                             |
| -------------------------------------- | ------------------------------------------------------------- | ------------------------------------------------------------------------------ |
| `help` / `help <group>` / `help all`   | `group`は上記のいずれか                                               | command group、またはすべてのcommandを表示                                                |
| `log-level [level]`                    | 省略時は現在値を表示。`off`、`error`、`info`、`debug`                       | runtime log levelを確認または変更。reset後は`info`へ戻る                                     |
| `rtc-get` / `rtc-status` / `rtc-clear` | なし                                                            | local RTCの取得、provider状態の表示、設定済みmarkerのclear                                    |
| `rtc-set YYYY-MM-DD hh:mm:ss`          | local timeを固定形式で指定                                            | RTCを設定し、read-backによって設定結果を確認。timezone/DST変換は行わない                               |
| `test-roundtrip [rounds]`              | `rounds`は`1..1000`。省略時はprofile依存でSmoke=1、Normal=10、Stress=100 | 8.3 file name、LFN、concurrencyを含むstorage test suiteを指定回数実行                      |
| `test-fatfs-time`                      | なし                                                            | RTCとFatFs timestampの一致を検証                                                      |
| `test-hotplug`                         | なし                                                            | 1 roundの中でremove、`NO_MEDIA`、reinsert、remountまでを検証。consoleの`ACTION REQUIRED`に従う |
| `model-info`                           | なし                                                            | `MTFSTEST.MTF`を認証し、信頼済みのmodel情報を表示                                             |
| `model-load`                           | なし                                                            | model API経由で`MTFSTEST.MTF`をloadし、内容を検証                                         |
| `model-hotplug`                        | なし                                                            | media removal時のcleanupと、再挿入後のrecoveryを検証                                       |
| `bench-info`                           | なし                                                            | targetのbenchmark条件のみを表示                                                        |
| `bench-smoke`                          | なし                                                            | 短時間の非破壊benchmarkを実行                                                            |
| `bench-normal`                         | なし                                                            | 1 MiB baselineの非破壊benchmarkを実行                                                 |
| `diag`                                 | なし                                                            | 直前のstorage commandから保持しているcommon/media/targetのsnapshotを表示                      |
| `diag-help`                            | なし                                                            | diagnostics snapshot、累積counter、reset対象範囲を説明                                    |
| `stack-highwater`                      | なし                                                            | coordinator/worker起動後のpeak stack使用量と残りmarginを表示                                |
| `test-diagnostics-reset`               | なし                                                            | activeなcontext上でdiagnostics resetのcontractを検証                                  |
| `diag-reset`                           | なし                                                            | I/Oやmedia状態を変更せず、diagnostic counterとreset epochのみをreset                        |
| `crypto-info`                          | なし                                                            | RAではRSIP、STではSAES/DHUKの実行条件とdiagnosticsを表示                                     |
| `crypto-consistency`                   | なし                                                            | provision済みkeyを使用し、AES-256-GCM処理の一貫性を検証                                        |
| `crypto-negative`                      | なし                                                            | tag/ciphertextなどを破損した入力が正しく拒否されることを検証                                          |
| `crypto-package-test`                  | なし                                                            | provision済みfleet keyを使ってsealed test packageを検証                                 |
| `model-negative`                       | なし                                                            | package、reader、policyに異常がある場合にmodel APIが正しく拒否することを検証                           |

RA版にはSPI diagnostics、RSIP、Ethos-U55 model testが含まれます。ST版にはSDMMC2 IDMA/polling、SDMMC diagnostics、BUSYD0END ready IRQ、SAES/DHUK、Neural-ART model testが含まれます。

最初の動作確認には`test-roundtrip 1`を使用します。通常の回帰確認では、各profileの既定値で`test-roundtrip`を実行します。

## `apps/key-provision`

fleet keyの初回登録と明示的な更新だけを行う専用applicationです。

信頼できるlocal UART sessionを使用して32-byteのraw keyをXMODEM-CRCで受信し、RAではRSIPでwrapしてOSPIへ、STではSAES/DHUKでwrapしてexternal NORへ保存します。

通常applicationへprovisioning commandを組み込むのではなく、対象board向けの専用projectをbuild/flashして使用します。

* [`apps/key-provision/targets/ek_ra8p1`](../apps/key-provision/targets/ek_ra8p1)
* [`apps/key-provision/targets/stm32n6570_dk`](../apps/key-provision/targets/stm32n6570_dk)

host側でのfleet key生成、sealed package作成、keyの取り扱いについては、先に[Sealed Model / Security guide](security.md#host-tools)を確認してください。

```text
help provisioning
info
provision-xmodem
update-xmodem
```

read-onlyの検証commandは、RA版では`verify-ospi`、ST版では`verify-nor`です。

実際に利用可能なcommandや表示内容については、実行中binaryの`help all`を最終的な確認先としてください。

key recordはgenerationとkey versionを持つdual-slot構成です。更新時にはinactive slotをerase/write/readbackし、最後にcommit wordを書き込みます。このため、更新途中で電源が切れた場合でも従来のslotを残せる設計になっています。

ただし、両slotの破損、whole-chip erase、NOR障害からの復旧までは保証しません。STM32N6570-DKでwhole-chip eraseを実行すると、`0x77ffe000`/`0x77fff000`にあるkey slotも消去されます。

### XMODEMでの初回登録

1. access controlされたhost上で`mtfs-keygen --output fleet.key`を実行するか、同等の管理環境でexact 32-byte binaryの`K_fleet` fileを用意します。hex text、Base64、改行付きfileは使用できません。
2. 専用provisionerを起動し、`info`と`help provisioning`でprovider、保存先、利用可能なcommandを確認します。
3. `provision-xmodem`を入力します。すでに有効なkeyが登録されている場合、このcommandは書き込み前に拒否されます。
4. `XMODEM-CRC ready`が表示されたら、60秒以内にterminal softwareのXMODEM送信を開始し、`fleet.key`をbinary fileとして送信します。consoleへのpasteやtext送信は行わないでください。128-byte/1 KiB blockの未使用領域には、XMODEM senderによる`0x00`または`0x1a`のpaddingのみ使用できます。
5. XMODEM receive、wrap、commit、readback crypto validationがすべて`PASS`になることを確認します。
6. RAでは`verify-ospi`、STでは`verify-nor`を実行し、generation、key ID/version、active slot、およびcrypto validationが`PASS`であることを再確認します。このverify処理はkey storeへ書き込みません。

送信中は、terminalのsession logging、remote console bridge、clipboard連携など、raw keyが別の媒体へ複製される可能性のある機能を無効にしてください。

UART/debug接続については[Getting Started](getting-started.md)を参照してください。

受信したraw keyはwrap処理直後にRAMからzeroizeされ、removable SDへ保存されることはありません。

### 明示更新

`update-xmodem`は、既存keyを意図的に置き換える場合にのみ使用します。

初回登録と同じ32-byte fileをXMODEM-CRCで送信し、inactive slotへのcommitとreadback検証が完了すると、key versionが増加します。

> **現行toolの制約:** `mtfs-seal`/`mtfs-test-package`がpackageへ記録するfleet key ID/versionは`1/1`固定です。
>
> 一方、`update-xmodem`成功後のactive recordはversion 2以降となり、target providerはpackageとrecordのversionが完全に一致することを要求します。
>
> このため、現行host toolで生成したpackageは、同じraw keyで再sealした場合でも、key更新後のtargetでは開くことができません。
>
> 対応するversionを指定できるpackagerとrotation/rollback手順が整うまでは、sealed packageを利用しているdeviceで`update-xmodem`を実行しないでください。

inactive slotへcommitする仕組みは、更新途中で失敗した場合に従来slotを保持するためのものです。version更新後のpackage互換性や、電源断、両slot破損、whole-chip eraseを含むあらゆる障害からのrollbackを保証するものではありません。

removable SDをfleet keyの保存先として使用しないでください。また、raw keyをrepository、command line、log、ELF、mapへ含めないでください。

production keyの生成、backup、配布については利用者側で適切に管理する必要があります。

## `apps/sentinel-lab`

Sentinel Labは、dataset収集、live monitor、CPU/NPUの一致確認、hotplug動作の検証を行うapplicationです。

### Console構文

両targetで共通して利用できるcommandです。

```console
help
help <group>
help all
log-level [off|error|info|debug]

record [samples]
pseudo-collect-delay-ramp [samples-per-stage] [seed]
pseudo-collect-hard-fault [samples] [seed]

sentinel-monitor [samples]
pseudo-monitor-delay-ramp [samples-per-stage] [seed]
sentinel-infer [iterations]
sentinel-infer-profile [iterations]
sentinel-infer-pseudo-slow [iterations]
sentinel-infer-hotplug [iterations]
sentinel-infer-vector HEX48 [iterations]
siv HEX48 [iterations]
sivb BASE64URL32 [iterations]
```

EK-RA8P1でのみ利用できるcommandです。

```console
sentinel-monitor-fallback-test [samples]
sentinel-ra-validate [repeats]
sentinel-ra-accept [repeats]
```

STM32N6570-DKでのみ利用できるcommandです。

```console
sentinel-monitor-hotplug [samples]
```

`<group>`には`general`、`sentinel`、`diagnostics`、`developer`を指定できます。

`record`、pseudo collection、RA validation/acceptanceを利用するには`MTFS_ENABLE_STORAGE_SENTINEL=1`が必要です。

monitor、inference、profile、hotplug、vector commandを利用するには、さらに`MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE=1`と`MTFS_ENABLE_SEALED_MODEL=1`が必要です。

そのため、現在のbuildで利用可能なcommandについては、実行中binaryの`help all`を最終的な確認先としてください。

### Dataset collection

| command                                                | 引数・既定値                                            | 動作                                                                            |
| ------------------------------------------------------ | ------------------------------------------------- | ----------------------------------------------------------------------------- |
| `record [samples]`                                     | 既定`0`。`0`はsample数を制限せず、I/O errorやresetなどで終了するまで継続 | test injectionを使用せず、実際のI/Oから得られたsampleをCSVとして出力                               |
| `pseudo-collect-delay-ramp [samples-per-stage] [seed]` | 既定`100 1`。両方とも`1`以上                               | baseline、light、medium、strong、recoveryの5 stageを順番に実行し、各stageで指定数のsampleをCSVへ出力 |
| `pseudo-collect-hard-fault [samples] [seed]`           | 既定`100 1`。両方とも`1`以上                               | sync I/Oのhard faultを明示的に注入し、指定数のsampleをCSVへ出力                                 |

`seed`はpseudo injectionを再現するために使用する符号なし整数です。

pseudo datasetには`scenario_origin=injected`が設定されます。test injectionを使用していない通常の`record` datasetとは混在させないでください。

### Monitor / inference

| command                                                | 引数・既定値                             | 動作                                                                                                                                        |
| ------------------------------------------------------ | ---------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| `sentinel-monitor [samples]`                           | RAは既定`10`かつ`1`以上。STは既定`0`で、`0`は無期限 | 32 windowのwarmup後、live windowをRULE/NPU/CPUで分類                                                                                             |
| `pseudo-monitor-delay-ramp [samples-per-stage] [seed]` | 既定`10 1`。両方とも`1`以上                 | 5 stageのdelay rampをmonitorし、遅延状態とrecovery時の分類経路を確認                                                                                        |
| `sentinel-infer [iterations]`                          | 既定`10`、`1`以上                       | 認証済みresident modelへ保持中のfeatureを入力し、CPU/NPUの数値結果が一致することを確認                                                                                 |
| `sentinel-infer-profile [iterations]`                  | 既定`10`、`1`以上                       | inferenceに加えてlatency、scheduler、stack、accelerator diagnosticsを表示                                                                           |
| `sentinel-infer-pseudo-slow [iterations]`              | 既定`10`、`1`以上                       | STでは直前のdelay-ramp collectionで保持したstrong-delay frameを評価。RAではbaseline-relative-v2がraw frameを直接分類しないため、`pseudo-monitor-delay-ramp`の実行を案内して終了 |
| `sentinel-infer-hotplug [iterations]`                  | 既定`10`、`1`以上                       | RAではresident modelを保持したmonitor pathでremove/reinsertとbaseline再構築を検証。STではinference pathでmedia removal中もresident modelを利用できることを検証            |
| `sentinel-monitor-fallback-test [samples]`             | RA限定。既定`1`、`1`以上                   | 許可されたNPU runtime failureだけがCPU fallbackへ進むcontractをtest injectionで検証                                                                      |
| `sentinel-monitor-hotplug [samples]`                   | ST限定。既定`10`、`1`以上                  | live monitor中のremove/`NO_MEDIA`/reinsertとbaseline再構築を検証                                                                                   |

`sentinel-infer`と`sentinel-infer-profile`を実行するには、あらかじめfeatureが保持されている必要があります。起動直後など、利用可能なfeatureがない場合は、先に`record 1`または`pseudo-collect-delay-ramp`を実行してください。

model packageはcommand開始時に認証し、その後monitorを実行している間はRAM上にresident modelとして保持されます。

media discontinuityやhard errorはAIを使用せずRULEで処理します。有効なwindowはNPUへ入力し、判定がambiguousな場合はCPU arbitrationを行います。CPU fallbackへ進むのは、許可されたNPU runtime errorが発生した場合だけです。

hotplug commandでは、consoleに表示される`ACTION REQUIRED`に従ってcardをremove/reinsertしてください。

終了時には、file、mount、provider、model、arenaの順序を考慮し、取得時とは逆順にcleanup/zeroizeします。

### Frozen vector / corpus

| command                                    | 引数・既定値                                                            | 動作                                                           |
| ------------------------------------------ | ----------------------------------------------------------------- | ------------------------------------------------------------ |
| `sentinel-infer-vector HEX48 [iterations]` | `HEX48`は24 byteを表す正確に48桁のhex。既定`10` iterations、`1`以上              | 24個のsigned Q4 inputをCPU/NPUへ入力し、結果の一致を検証                     |
| `siv HEX48 [iterations]`                   | 上記と同じ                                                             | `sentinel-infer-vector`の短縮alias                              |
| `sivb BASE64URL32 [iterations]`            | `BASE64URL32`は24 byteを表すpaddingなしの正確に32文字。既定`10` iterations、`1`以上 | Base64URLで表現した同じ24個のsigned Q4 inputを検証                       |
| `sentinel-ra-validate [repeats]`           | RA限定。既定`2`、`1`以上                                                  | validation-onlyのEthos-U corpusを指定回数実行し、characterization結果を表示 |
| `sentinel-ra-accept [repeats]`             | RA限定。既定`2`、`1`以上                                                  | frozen contract v2のheld-out acceptance corpusを指定回数実行         |
