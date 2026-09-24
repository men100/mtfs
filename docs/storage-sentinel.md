# Storage Sentinel guide

Storage Sentinelは、通常のstorage I/Oから得られる統計をpassiveに観測し、同じSD card/mount session内で確立したbaselineからの変化を検出するoptional featureです。

観測のために追加のprobe read/writeを発行することはなく、既存I/Oのlatency、分布、error、media lifecycleを利用します。

判定結果の`ANOMALY`は、「現在のI/O統計が、学習済みのnormal patternから逸脱している」ことを示します。

SD cardの寿命、残寿命、故障原因、故障時期を直接推定するものではありません。また、`NORMAL`であってもcardの健全性を保証するものではありません。

Storage Sentinelは、card交換などの判断を単独で自動化するための機能ではありません。diagnostics、application log、media状態などと組み合わせて、異常の兆候を調査するための仕組みです。

## 全体の処理

Storage Sentinelでは、不完全なdataや明らかなstorage errorをAIへ渡さず、まずdeterministic ruleで処理します。

AIによる判定へ進むのは、必要な情報が揃っており、同じsession内で連続して取得された有効なwindowだけです。

```text
applicationのread/write/sync
  → passive observerが既存I/Oを1回だけdownstreamへ委譲して時間を記録
  → diagnostics/timing snapshot間の差分からfeature windowを生成
  → rule-based guardでmedia、error、連続性、data量を検査
  → session開始後のwarmupでcard固有baselineを確立
  → baseline-relative preprocessing v2で24要素のQ4 inputへ変換
  → target固有NPUで再構成し、score区間をthresholdと比較
  → threshold付近だけcanonical CPUでarbitration
  → NORMAL/ANOMALY、またはRULE由来の状態を報告
```

inferenceを有効にしない構成でも、passive observer、version付きfeature、dataset recorderは利用できます。

feature switchと必要なsourceについては[Configuration reference](configuration.md#feature-switch)および[Source integration guide](integration.md#storage-sentinel)を参照してください。

## 用語

| 用語                | このguideでの意味                                                       |
| ----------------- | ----------------------------------------------------------------- |
| snapshot          | ある時点の累積diagnostics/timing counterをcopyしたもの。取得時にmedia I/Oは発生しない    |
| feature window    | 連続する2つのsnapshot間で発生したI/Oを差分として表した1つの観測区間                          |
| feature schema    | windowを構成するfield、単位、順序を定義するversion付きcontract                      |
| raw feature       | feature schema v1からmodel入力用にencodeした24個の非負整数                      |
| warmup            | 現在のcard/sessionにおける通常値を求めるため、inference開始前に有効なwindowを蓄積する期間        |
| baseline          | warmup中に得られたwindowから、featureごとに求めた中央値                             |
| baseline-relative | 絶対値ではなく、そのsessionのbaselineからどれだけ変化したかをmodel入力として使用する方式            |
| preprocessing     | raw featureをbaseline相対値へ変換し、modelへ渡せる固定長・固定範囲の値へ整える処理             |
| normalization     | targetごとのscaleを使ってfeatureの大きさを揃え、値の大きいfeatureだけがmodelを支配することを防ぐ処理 |
| quantization      | 浮動小数点値をscaleとzero pointに基づいて整数へ変換し、組み込みCPU/NPUで扱える形式にする処理         |
| Q4                | 実数値を16倍し、signed 8-bit整数として保持する共通固定小数点表現                           |
| autoencoder       | 入力を小さな内部表現へ圧縮してから元の形へ再構成し、その再構成誤差によって逸脱を測るmodel                   |
| saturation        | Q4への変換結果が`-128..127`を超え、上限または下限の値へclampされた状態                      |
| OOD               | Out Of Distribution。学習・検証した入力範囲から外れており、modelの判定を採用しない状態           |
| rule-based guard  | AIによる判定より前に、media抜去、error、data不足、不連続などをdeterministicに処理する仕組み      |
| score             | model入力と再構成結果との差を表す値。大きいほどnormal patternから離れている                   |
| threshold         | scoreをnormal/anomalyへ分けるtarget固有の境界値                              |
| CPU arbitration   | NPU誤差を考慮したscore区間がthresholdを跨ぐ場合に、canonical CPUで最終判定を確定する処理       |
| CPU fallback      | NPU runtime error時の回復経路。threshold付近を確定するCPU arbitrationとは別の処理     |
| resident model    | 認証済みpackageからRAMへloadし、monitor中は再読込みせず保持するmodel                   |
| canonical TFLite  | target NPU形式へ変換する前のint8 model。再現性確認とCPU側の基準判定に使用する                |
| pseudo data       | test injectorによって意図的にdelayなどを加えた検証用data。自然発生した異常とは区別して扱う          |

## Passive observationとfeature window

`mtfs_sentinel_observer_t`は、既存の`mtfs_block_device_t`をwrapします。

read、write、syncをdownstream deviceへ1回だけ委譲し、その処理時間を単調増加するmicrosecond clockで計測します。

観測のために同じsectorを読み直したり、diagnostics用のfileを書き込んだりすることはありません。そのため、Storage Sentinelの観測そのものがmediaへの追加I/Oやwearを発生させる構造にはなっていません。

ただし、clock取得、counter更新、lock、inferenceにはCPU時間とRAMを使用します。

observerはread/write/syncごとに、sample数、合計latency、無効sample数、22 bucketのlatency histogramを累積します。

Storage Sentinel coreは、common block diagnostics、observer、optionalなtarget transport diagnostics、cache済みmedia metadataについて、現在のsnapshotと前回のsnapshotを比較し、1つの`mtfs_sentinel_feature_v1_t`を生成します。

この差分がfeature windowです。

featureには次の情報が含まれます。

* observation interval、target/transport ID、media generation
* read/write/syncの呼び出し回数、成功数、失敗数、要求/完了sector数
* operationごとのsample数、平均latency、latency histogram
* common error counter、およびoptionalなtransport error/timeout/abort/clock error
* diagnostics/observer reset epoch、insert/remove/media error event
* validity mask、およびdata不足や不連続を示すflag

counter reset、counter飽和、clockの逆行、target/transport変更、media generation変更などにより安全に差分を計算できない場合、そのwindowは不連続または無効として扱います。

取得処理と公開型の詳細については、[API Reference](https://men100.github.io/mtfs/index.html)の`mtfs_sentinel` groupを参照してください。

## Feature schema v1

`mtfs_sentinel_feature_v1_t`は観測情報を幅広く保持しますが、そのすべてをmodelへ直接入力するわけではありません。

`mtfs_sentinel_feature_encode_raw()`は、validity、flag、hard error、timing整合性を確認したうえで、次の24要素へ固定された順序でencodeします。

| operation/関係 | 要素数 | 内容                                                |
| ------------ | --: | ------------------------------------------------- |
| read         |   7 | 平均latency、無効timing比率、latency histogramを5帯域に集約した比率 |
| write        |   7 | readと同じ7要素                                        |
| sync         |   7 | readと同じ7要素                                        |
| workload構成   |   3 | 全timing sampleに対するread/write/syncの構成比             |

比率はpermille、平均latencyはmicrosecond単位です。

各operationの22 histogram bucketは、`0..3`、`4..7`、`8..11`、`12..14`、`15..21`の5帯域へまとめます。

これにより、平均latencyだけでは捉えにくいlatency tailの増加もmodel入力へ反映できます。

featureの完全な順序、範囲、単位は[`feature_schema_v1.json`](../tools/storage_sentinel/feature_schema_v1.json)で定義されています。

target ID、timestamp、media generation、label、command、injection条件はmodel入力には含めません。

また、I/O errorやtimeoutなどのhard fault counterをmodelに推定させることはせず、rule-based guardで直接処理します。既知のstorage errorを曖昧なAI scoreへ変換しないためです。

24要素のschema自体はtarget間で共通ですが、公開modelが実際に使用するactive featureは、capture結果に基づいてtargetごとに固定されています。

| target        | active feature                                                                                                        |
| ------------- | --------------------------------------------------------------------------------------------------------------------- |
| EK-RA8P1      | read平均latency、read histogram `8..11`/`15..21`、write平均latency、write histogram `12..14`/`15..21`                        |
| STM32N6570-DK | read平均latency、read histogram `8..11`/`12..14`/`15..21`、write平均latency、write histogram `12..14`/`15..21`、sync平均latency |

使用しないfeatureをschemaから削除するのではなく、target policyの`active_feature_mask`によって0へ固定します。

これにより、schema互換性を維持しながら、各targetで有効性が確認されたsignalだけをmodelへ入力できます。

`active_feature_mask`をapplication側で任意に変更すると、公開modelが前提とする入力contractと一致しなくなります。

schema versionはfeatureの並びと意味を固定するversionです。後述するpreprocessing versionやmodel versionとは別に管理されます。

必要fieldが不足しているschema、timing sampleが存在しないwindow、histogram合計とsample数が一致しないwindowはinference対象になりません。

## Rule-based guard

monitorはmodelを実行する前に、次の状態を上から順に評価します。

| 状態                | 主な条件                                                     | 意味と対応                                  |
| ----------------- | -------------------------------------------------------- | -------------------------------------- |
| `NO_MEDIA`        | Card Detectがabsent、`NO_MEDIA`、またはno-media error          | mediaが存在しないためinferenceを停止              |
| `NOT_READY`       | 挿入/再初期化中、または`NOT_READY`                                  | initialize/mountの完了を待つ                 |
| `BLOCK_ERROR`     | workload失敗、operation failure、I/O/timeout/transport error | AIではなくstorage errorとして調査               |
| `INVALID`         | schema/size不一致、required validity不足、counter飽和             | 入力contractを満たしていないため破棄                 |
| `DISCONTINUITY`   | media generation変更、reset epoch変更など                       | 以前のwindowやbaselineとの連続性が失われている         |
| `INSUFFICIENT`    | operation sample不足                                       | 次の有効なI/O windowを待つ                     |
| `WARMUP`          | windowは有効だがbaseline未完成                                   | baseline進捗を表示し、まだinferenceを行わない        |
| `OOD`             | baseline-relative Q4変換で許容数を超えるsaturationが発生              | 学習範囲外としてAI判定を採用しない                     |
| `INFERENCE_ERROR` | preprocessingまたはCPU/NPU処理を完了できない                         | runtime/memory/provider diagnosticsを確認 |

これらの結果は、console上では`source=RULE`として表示されます。

`RULE`は「AIが異常と判定した」という意味ではありません。AIによる判定へ進めない理由をdeterministicに分類したことを示します。

model scoreに基づく分類は、`NORMAL`と`ANOMALY`だけです。

## Baseline-relative preprocessing v2

### なぜcard/sessionごとのbaselineを使うのか

SD cardの通常latencyは、card controller、容量、使用履歴、filesystem上の配置、transport、clock、build profileなどによって異なります。

絶対latencyだけをmodelへ入力すると、正常な別cardを異常と誤判定しやすくなります。

baseline-relative方式では、現在のcardが現在の実行条件で示した通常値を基準とし、そこからの相対的な変化を評価します。

baselineはcardの恒久的な健康状態を表す情報ではありません。現在のmedia generationに対してのみ有効なsession stateです。

別のcardへ引き継いだり、card再挿入前のbaselineをそのまま再利用したりすることはありません。

### Warmupとbaseline確立

公開RA/ST profileでは、いずれも32個の有効なwindowをwarmupに使用します。

実装上は8、16、32 windowのpolicyを受け付けますが、公開artifactと同じ判定結果を再現する場合は、bundleに格納されている32-window policyを変更しないでください。

warmupに使用するのは、次の条件をすべて満たす通常windowだけです。

* 同じnon-zero media generationに属する
* required validityを満たす
* timingに矛盾がない
* hard errorがない

`pseudo-*` commandによるinjection中のwindowは、baselineが意図的に異常側へ移動することを防ぐためwarmup対象から除外します。

32 windowが揃うと、24個のfeatureそれぞれについて中央値を求めます。

偶数個の中央値を求める場合は、中央2値のmidpointを切り上げる固定規則を使用します。

baseline完成後は値を追従更新しません。

異常状態が長期間継続した場合に、その状態を新しい「正常」として取り込んでしまうことを防ぐためです。

baselineはresetまたはmedia generation変更まで固定されます。

### 相対変換

feature `i`のraw値を`x_i`、warmupで求めた中央値を`b_i`とします。

read/write/syncの平均latencyは、baselineに対するpermille単位の変化量へ変換します。

```text
relative_i = round_away((x_i - b_i) * 1000 / max(b_i, latency_floor_i))
```

`latency_floor_i`は、baselineが0または極端に小さい場合でも除算を安定させるために使用します。

それ以外のactive featureは、もともとpermille単位で表された分布/構成比なので、baselineとの差分をそのままsigned valueとして使用します。

```text
relative_i = x_i - b_i
```

targetごとの`active_feature_mask`に含まれないfeatureは0へ固定します。

active featureは、学習・検証dataから固定した`relative_scale_floor_i`を使って共通Q4形式へscaleします。

```text
input_q4_i = clamp(round_away(relative_i * 16 / relative_scale_floor_i), -128, 127)
```

たとえばQ4の`16`は概念上の`1.0`、`-32`は`-2.0`に相当します。

浮動小数点演算やheapを使用せず、Hostとtargetで同じ丸め結果が得られるよう整数演算で定義しています。

Q4範囲を超えたfeatureは端値へsaturationし、そのfeatureをmaskへ記録します。

公開RA/ST policyでは`maximum_saturated_features`が0のため、1要素でもsaturationするとOODになります。

OODを無理に`ANOMALY`として扱うのではなく、入力がmodelの想定範囲外だったことを明示的に報告します。

「v2」は、baseline推定、相対変換、scale、丸め、saturation、baseline更新policyをまとめたpreprocessing contractのversionです。

feature schema v1、bundle version、model version、NPU runtime versionとはそれぞれ独立しています。

targetごとの具体的な値は、各targetの[`preprocessing.json`](../artifacts/storage_sentinel/reference/ek_ra8p1/training/preprocessing.json)および[`preprocessing.json`](../artifacts/storage_sentinel/reference/stm32n6570_dk/training/preprocessing.json)に記録されています。

## Model、score、CPU/NPU hybrid判定

公開modelは、24要素の入力を`24 → 12 → 4 → 12 → 24`で再構成する小型autoencoderです。

normal training windowのpatternを圧縮・再構成し、元の入力と再構成結果との差をanomaly scoreとして使用します。

このmodelは、個別の故障原因を分類するmulti-class classifierではありません。

共通Q4 inputを`x_i`、再構成outputを`y_i`とすると、scoreは24要素の二乗誤差平均をQ8形式で表した値です。

```text
score_q8 = (sum((x_i - y_i) * (x_i - y_i)) + 12) / 24
```

scoreがtarget固有のthreshold以下であればnormal、thresholdを超えればanomalyと判定します。

thresholdはnormal validation dataの分布から固定し、bundle内に格納します。

RAとSTでは、transport、model、preprocessing policy、thresholdが異なります。そのため、score値やlatencyをtarget間で直接比較したり、rankingへ使用したりすることはできません。

### NPUの数値誤差を曖昧なままにしない

NPU backendでは、canonical CPUと比較して数LSB程度のquantization差が発生する場合があります。

Storage Sentinelでは、単一のNPU scoreをそのままthresholdと比較するのではなく、acceptanceで固定したper-element誤差上限から、取り得る`score_min_q8..score_max_q8`を求めます。

| score区間とthreshold              | 判定経路                                             |
| ------------------------------ | ------------------------------------------------ |
| `score_max_q8 <= threshold_q8` | score区間全体がnormal側にあるため、NPUだけでnormalを確定           |
| `score_min_q8 > threshold_q8`  | score区間全体がanomaly側にあるため、NPUだけでanomalyを確定         |
| それ以外                           | score区間がthresholdを跨ぐため、canonical CPUでarbitration |

CPU arbitrationは多数決ではありません。

threshold付近でNPUの数値誤差によって結果が変わる可能性がある場合にだけ、canonical CPUの結果をapplication-visibleな最終判定として採用します。

RAの公開acceptanceではcommon-Q4誤差上限は0、STのaccepted profileでは1です。

CPU fallbackは、CPU arbitrationとは別の経路です。

NPU runtimeがapplication policyでfallback可能とされているerrorを返した場合に、CPUへ切り替えて処理を継続します。

通常のthreshold境界処理は`CPU-ARBITRATION`、runtime障害からの回復は`CPU-FALLBACK`として、それぞれ個別にcount/表示されます。

CPUでも処理を完了できない場合は`INFERENCE_ERROR`になります。

## 判定結果の読み方

monitor出力では、少なくとも`state`と`source`を組み合わせて確認してください。

| 例                                             | 解釈                                              |
| --------------------------------------------- | ----------------------------------------------- |
| `state=NORMAL source=NPU`                     | NPUから求めたscore区間全体がnormal側だった                    |
| `state=ANOMALY source=NPU`                    | NPUから求めたscore区間全体がanomaly側だった                   |
| `state=NORMAL/ANOMALY source=CPU-ARBITRATION` | threshold付近だったためCPUで最終判定を確定した                   |
| `state=NORMAL/ANOMALY source=CPU-FALLBACK`    | NPU runtime error発生後、CPUへ切り替えて処理を完了した           |
| `state=WARMUP source=RULE`                    | baselineを収集中。`baseline_progress/required`を確認する  |
| `state=OOD source=RULE`                       | 入力がpreprocessingの許容範囲外。saturation maskを確認する     |
| `state=BLOCK_ERROR source=RULE`               | storage errorを直接検出。`diag`でerror/timeoutを確認する    |
| `state=DISCONTINUITY source=RULE`             | snapshotまたはmedia sessionの連続性が失われている。warmupをやり直す |

`ANOMALY`だけを根拠にcard交換やdata消去を実行しないでください。

まずcommon/media/target diagnostics、workload、電源、配線、clock、hotplug履歴を確認します。

diagnosticsの読み方については[Storage operations](storage-operations.md#diagnostics)を参照してください。

## Resident modelとhotplug

monitor開始時にsealed `SENTINEL.MTF`を認証し、target/transport/accelerator ID、bundle layout、runtime memory policyを検証したうえで、呼び出し側が用意したRAMへloadします。

monitor中はmodelをresidentとしてRAM上に保持するため、windowごとにSD cardからmodelを読み直すことはありません。

model packageの暗号化、認証、key boundaryについては[Sealed Model / Security guide](security.md)を参照してください。

cardを抜去すると、monitorは新しいinferenceの開始を停止し、`NO_MEDIA source=RULE`を報告します。

Sentinel Labのhotplug pathでは、認証済みresident modelをRAM上に保持できます。ただし、以前のcardで確立したbaselineをそのまま使用してinferenceを継続することはありません。

cardを再挿入した後は、initialize、register、mountをやり直し、新しいmedia generationに対してwarmupとbaseline確立を再実行してからclassificationを再開します。

終了時には、file、mount、provider、model、arenaの依存関係を考慮し、取得時とは逆の順序でcloseします。

plaintext model、runtime buffer、CPU work、temporary stateはzeroizeします。

resident modelは認証済みであっても秘密情報を含む可能性があるため、不要になった後も通常のstatic dataとして残したままにはしません。

## Natural dataとpseudo data

`record`は、実際のI/Oから自然に得られたwindowを収集します。

`pseudo-*` commandは、test injectorによって再現可能なdelayなどを意図的に加え、medium/strong/recovery状態や各判定経路を検証します。

pseudo dataは、「実際のcard故障を観測したdata」ではありません。

natural dataとpseudo dataは、CSVのcondition、scenario origin、injection metadata、sidecar manifestによって区別されます。

injection中に取得したwindowはbaseline warmupへ使用しません。

datasetを利用する場合も、pseudo labelを自然発生した故障のground truthとして解釈したり、通常の`record` datasetと混在させたりしないでください。

commandと引数については[Sentinel Lab manual](applications.md#appssentinel-lab)を参照してください。

## 正式reference profile

| target        | transport             | accelerator | 正式profile         |
| ------------- | --------------------- | ----------- | ----------------- |
| EK-RA8P1      | SPI                   | Ethos-U55   | optimized Release |
| STM32N6570-DK | SDMMC2 4-bit IDMA+IRQ | Neural-ART  | optimized Release |

Debug buildでは、instruction timing、log、optimization、stack/memory behaviorがRelease buildと異なります。

また、STのpolling fallbackはIDMA＋IRQとはcompletion behaviorが異なります。

いずれも機能確認やdiagnosticsには使用できますが、公開modelのclassification qualityを評価する正式なreference profileではありません。

RAとSTは同じ24要素schemaと判定原則を共有しますが、capture、active feature、baseline scale、model、threshold、NPU runtimeはtargetごとに異なります。

そのため、RA用packageをSTへ、またはST用packageをRAへ流用することはできません。

bundle parserはtarget、transport、accelerator、model format、profile IDを照合し、一致しないpackageを拒否します。

現在のacceptance結果とresource使用量については[Performance](performance.md#storage-sentinel-reference)を参照してください。

## Dataset splitと再現性

公開datasetは、row単位でrandom splitしていません。

cardとcapture sessionをsplitの単位とすることで、同じcard/sessionから得られた近接windowがtrainingとevaluationの両方へ含まれ、performanceを過大評価することを防いでいます。

| card     | role       | 使用目的                                              |
| -------- | ---------- | ------------------------------------------------- |
| Card A/B | training   | normal patternからmodelを学習                          |
| Card C   | validation | preprocessing、threshold、pseudo検出、recovery条件を選定・確認 |
| Card D   | held-out   | training/選定完了後に、固定条件で最終確認                         |

dataset indexには、target、split、card ID、session ID、condition、row数、payload/sidecar hashを記録します。

`row_repartitioning`は`false`です。

公開auditでは、全23 sessionについてcard/session単位の境界が維持されていることを確認します。

## 公開artifactの構成

[`artifacts/storage_sentinel`](../artifacts/storage_sentinel)は、次の用途ごとに構成されています。

| path                                | 内容                                                                 |
| ----------------------------------- | ------------------------------------------------------------------ |
| `datasets/<target>/<split>/<card>/` | CSV captureとsidecar manifest                                       |
| `reference/common/`                 | 評価済みfeature schema                                                 |
| `reference/<target>/training/`      | model、normalization、preprocessing、threshold、test vector、provenance |
| `reference/<target>/freeze/`        | held-out/NPU acceptance条件を固定した記録                                   |
| `models/<target>/`                  | 公開可能なcanonical TFLiteとtarget別artifact                              |
| `numerical/<target>/`               | CPU/NPUの数値一致を確認するcorpusとindex                                      |
| `audit/`                            | capture変換、clean reproduction、ST recipe、公開範囲に関する監査記録                |
| `environment/`                      | Python/Vela dependencyのlock fileとversion情報                         |
| `dataset-index.json`                | 公開session一覧とsplit contract                                         |
| `license-provenance.json`           | dataset、model、tool、生成物の由来と公開判断                                     |
| `SHA256SUMS`                        | 公開artifact全体のhash index                                            |

環境はCPython 3.10.6、TensorFlow 2.18.1、NumPy 2.0.2、Keras 3.12.4、seed 4303で固定しています。

RAでのVela変換にはArm Vela 5.1.0を使用します。

Python package自体はrepositoryへ収録せず、lock fileから別環境を構築します。

```console
python -m venv .venv-sentinel
. .venv-sentinel/bin/activate
python -m pip install -r artifacts/storage_sentinel/environment/requirements-lock.txt
python tools/storage_sentinel/reproduce_release.py --output-root <new-empty-output-directory>
```

基本commandでは、RA/STのtrainingとcanonical TFLite生成を2つの独立したworkspaceで実行し、生成結果がbyte単位で一致し、固定されたhashとも一致することを確認します。

`--output-root`には空のdirectoryを指定する必要があります。

RAのVela outputとbundleまで再現する場合は、Vela 5.1.0を導入した別環境を指定します。

```console
python tools/storage_sentinel/reproduce_release.py \
  --output-root <new-empty-output-directory> \
  --vela <path-to-vela-command> \
  --vela-python <path-to-vela-environment-python>
```

ST向けの生成まで行う場合は、利用者が正規に導入したST Edge AI Core 4.0.1-20581（atonn 1.1.3-275）と、同製品に同梱されている`test-int2` profileを指定します。

必要に応じてGNU tool directoryを`--st-tool-path`で追加します。

```console
python tools/storage_sentinel/reproduce_release.py \
  --output-root <new-empty-output-directory> \
  --stedgeai <path-to-stedgeai> \
  --st-reloc-profile <ST-Edge-AI-Core-4.0.1>/scripts/N6_reloc/test/neural_art_reloc.json \
  --st-reloc-profile-name test-int2
```

accepted `test-int2` profileはproject-owned fileではなく、ST Edge AI Coreに同梱されたvendor fileです。

そのためpublic repositoryにはcopyしておらず、public checkoutだけではST runtimeを生成できません。

一方で、利用者自身が正規に導入した上記versionのvendor tool/profileをexternal prerequisiteとして指定すれば、accepted runtime、bundle、必要に応じて`SENTINEL.MTF`までfail-closedで生成できます。

別version、別profile、変更したmemory layout、custom Neural-ART runtime、custom acceptance contractは、このreference workflowの対象外です。

公開している[`st-edge-ai-recipe-result.json`](../artifacts/storage_sentinel/audit/st-edge-ai-recipe-result.json)には、acceptance時のtool version、runtime hash/size、private temporary outputとの一致結果を記録しています。

公開treeの整合性を監査するcommandは次のとおりです。

```console
python tools/storage_sentinel/audit_public_release.py --repo-root .
```

このscriptは検査だけでなく、`dataset-index.json`、`audit/audit-summary.json`、`SHA256SUMS`も更新します。

内容の確認だけを目的とする場合でもclean worktree上で実行し、生成された差分をreviewしてください。

## STM32N6570-DK用`SENTINEL.MTF`の生成

ST用の`SENTINEL.MTF`は、canonical TFLite、利用者環境で生成したNeural-ART runtime、CPU/NPUの数値acceptance contractを1つのSentinel bundleへまとめ、そのbundleをdeviceへprovisionしたfleet keyでsealして作成します。

この手順で生成される`network_rel.bin`、`sentinel.bundle`、`SENTINEL.MTF`には、利用者が取得したST softwareから生成されたruntimeが含まれます。

適用されるlicenseの利用条件・再配布条件を確認し、public repositoryへcommitしないでください。

### 前提条件

次を用意します。

* [公開再現セット](#公開artifactの構成)のPython環境
* ST Edge AI Core 4.0.1-20581/atonn 1.1.3-275
* ST Edge AI Coreに同梱された`scripts/N6_reloc/test/neural_art_reloc.json`内の`test-int2` profile
* `mtfs-seal`/`mtfs-verify`/`mtfs-unseal`を同じdirectoryへbuild済みのHost環境
* targetの`apps/key-provision`で登録したものと同一の32-byte fleet key file
* 生成するruntimeのhashに対応する、事前に固定・評価済みのNPU acceptance contract

公開されている[`npu-acceptance-v3-compact.json`](../artifacts/storage_sentinel/reference/stm32n6570_dk/freeze/npu-acceptance-v3-compact.json)は、accepted `test-int2` relocation profileから生成し、target上で評価したruntimeだけに対応しています。

別のprofileから生成したruntimeへ流用しないでください。

public repositoryにはvendor profile自体を収録していません。

任意のrelocation profileを指定し、公開acceptance contractだけを流用することはできません。

custom profileを使用する場合は、そのruntimeに対する新しいtarget evaluationとacceptance freezeが必要です。ただし、そのworkflowは今回のpublic reference reproductionではサポートしていません。

### 1. Canonical model、Neural-ART runtime、bundleを生成する

repository rootで、空のdirectoryを`--output-root`に指定して実行します。

```console
python tools/storage_sentinel/reproduce_release.py \
  --output-root <new-empty-output-directory> \
  --stedgeai <path-to-stedgeai> \
  --st-reloc-profile <ST-Edge-AI-Core-4.0.1>/scripts/N6_reloc/test/neural_art_reloc.json \
  --st-reloc-profile-name test-int2 \
  --st-tool-path <path-to-required-gnu-tools>
```

必要なGNU toolがすでに`PATH`に含まれている場合、`--st-tool-path`は省略できます。

以降、`<st-root>`は次のdirectoryを表します。

```text
<new-empty-output-directory>/workspace-1/stm32n6570_dk
```

生成後、少なくとも次のfileが存在することを確認します。

| file                                                                     | 内容                                                                     |
| ------------------------------------------------------------------------ | ---------------------------------------------------------------------- |
| `<st-root>/training/`                                                    | bundleへ格納するmodel、normalization、preprocessing、thresholdのsource artifact |
| `<st-root>/sentinel_baseline_v2.tflite`                                  | canonical full-int8 TFLite                                             |
| `<st-root>/stedgeai-private-output/generated/network_rel.bin`            | Neural-ART relocatable runtime                                         |
| `<st-root>/stedgeai-private-output/sentinel_st_neural_art_manifest.json` | runtime ABI、memory region、quantization、hashを記録したmanifest               |
| `<st-root>/sentinel.bundle`                                              | identityとinner layoutを検証済みのCPU＋Neural-ART bundle                       |

scriptは、canonical TFLite、ST Edge AI Core/atonn version、profile名、runtime size/hash、conversion manifest、公開acceptance contractを順に検証します。

すべて一致した場合にのみ`pack.py`を実行し、bundleのtarget、transport、accelerator、model format、runtime count、required RAM、inner runtime identityを再度検証します。

いずれかが一致しない場合、bundleは生成されません。

公開用training manifestでは、dataset pathをrepository-relativeな表現へ置き換えています。

一方、freeze済みのST acceptanceは、同じdataset/session/file hashを持つ評価時artifact indexのidentity `b27f6bde9199ea3cf9271c1533d0ee0e67a87c02860a2a186c0c353419fc19c9`に紐付いています。

`reproduce_release.py`は、このpathだけのprovenance差分を明示的にconverterへ渡し、accepted conversion manifest identityを再現します。

これはdataset、model、normalizer、threshold、runtime、acceptance contractを書き換える処理ではありません。

生成されるmanifest自体はpath-neutralであり、絶対pathが含まれている場合は拒否されます。

### 2. Runtimeとacceptance contractのidentityを確認する

公開acceptance contractを使用する場合は、生成したruntimeとconversion manifestが、固定済みのidentityと一致している必要があります。

runtimeのSHA-256は次のcommandで確認できます。

```console
python -c "import hashlib,pathlib,sys; p=pathlib.Path(sys.argv[1]); print(hashlib.sha256(p.read_bytes()).hexdigest())" \
  <st-root>/stedgeai-private-output/generated/network_rel.bin
```

accepted `test-int2` runtimeの期待値は次のとおりです。

```text
dfbe8918b53f6570b81d73b468a1ba6d78d9a250e4fb0c005e705d8f80140c6e
```

conversion manifestのidentityは次のcommandで確認します。

```console
python -c "import json,pathlib,sys; print(json.loads(pathlib.Path(sys.argv[1]).read_text(encoding='utf-8'))['conversion_manifest_sha256'])" \
  <st-root>/stedgeai-private-output/sentinel_st_neural_art_manifest.json
```

公開acceptance contractに対応する期待値は次のとおりです。

```text
a037c2365228fd7bca2d9d2d2deb37788c26bcde283aa944a6b98569446ac09b
```

どちらか一方でも一致しない場合は、公開acceptance contractを指定したまま次の手順へ進まないでください。

tool versionが一致していても、relocation profileやmemory layoutが異なればruntime identityは変わります。

### 3. Sentinel bundleを作成する

`reproduce_release.py`は、identityが一致したことを確認した後、次と同等の`pack.py`処理を自動的に実行します。

個別調査やauditのために手動で実行する場合は、次のcommandを使用します。

```console
python tools/storage_sentinel/pack.py \
  --artifact <st-root>/training \
  --canonical-tflite <st-root>/sentinel_baseline_v2.tflite \
  --npu-binary <st-root>/stedgeai-private-output/generated/network_rel.bin \
  --npu-manifest <st-root>/stedgeai-private-output/sentinel_st_neural_art_manifest.json \
  --npu-acceptance artifacts/storage_sentinel/reference/stm32n6570_dk/freeze/npu-acceptance-v3-compact.json \
  --accelerator-id 0x4e415254 \
  --provenance-root . \
  --output <st-root>/sentinel.bundle \
  > <st-root>/sentinel.bundle.summary.json
```

`pack.py`は、canonical model、runtime、conversion manifest、acceptance contractのidentityに加え、target/transport/accelerator policyも検証します。

不一致が検出された場合は、acceptance checkを無効化したりmanifestを書き換えたりせず、runtime生成またはacceptanceの手順へ戻ってください。

生成されたsummaryを確認します。

```console
python -c "import json,pathlib,sys; d=json.loads(pathlib.Path(sys.argv[1]).read_text(encoding='utf-8')); print('target_id=0x%08x accelerator_id=0x%08x model_format=0x%08x runtime_count=%d required_ram=%d alignment=%d' % (d['target_id'],d['accelerator_id'],d['model_format'],d['runtime_count'],d['minimum_required_ram_32bit'],d['required_alignment']))" \
  <st-root>/sentinel.bundle.summary.json
```

STのCPU/NPU hybrid bundleでは、少なくとも次の値が表示されます。

| field            | 期待値                   |
| ---------------- | --------------------- |
| `target_id`      | `0x53544e36` (`STN6`) |
| `accelerator_id` | `0x4e415254` (`NART`) |
| `model_format`   | `0x534e5431` (`SNT1`) |
| `runtime_count`  | `2`（CPUとNeural-ART）   |

`required_ram`と`alignment`には、生成されたbundleのmemory planから求められた値を使用します。

公開recipeから生成される現在のpath-neutralなCPU＋NPU bundleは24,868 bytesで、bundle SHA-256は`c58d9f7a022f65ad586425d68a709389073c842987682c39b9c1c3d55a723926`、`required_ram`は32,592 bytesです。

scriptは、これらの値についても固定identityとして検証します。

評価時に使用した旧bundleのidentityには絶対pathを含むprovenanceが影響しているため、現在のpublic reference identityとしては使用しません。

model、runtime、conversion manifest、acceptance contractのidentity自体は変更されていません。

### 4. Fleet keyでsealする

Host toolをまだbuildしていない場合は、次を実行します。

```console
cmake -S tools/sealed_model -B build/sealed-model -DCMAKE_BUILD_TYPE=Release
cmake --build build/sealed-model --parallel
ctest --test-dir build/sealed-model --output-on-failure
```

生成からsealまでを1回のreference workflowで実行する場合は、`reproduce_release.py`へbuild済みの`mtfs-seal`、fleet key、出力先を指定します。

同じdirectoryにある`mtfs-verify`と`mtfs-unseal`も自動的に使用されます。

```console
python tools/storage_sentinel/reproduce_release.py \
  --output-root <new-empty-output-directory-outside-repository> \
  --stedgeai <ST-Edge-AI-Core-4.0.1>/Utilities/windows/stedgeai.exe \
  --st-reloc-profile <ST-Edge-AI-Core-4.0.1>/scripts/N6_reloc/test/neural_art_reloc.json \
  --st-reloc-profile-name test-int2 \
  --mtfs-seal <sealed-tools-directory>/mtfs-seal \
  --fleet-key <fleet-key-file> \
  --sealed-output <output-directory-outside-repository>/SENTINEL.MTF
```

public demo keyを使用して検証する場合は、次の注意を必ず守ってください。

> **TEST/DEMO KEY - PUBLIC AND NOT SECRET - DO NOT USE IN PRODUCTION**

scriptは、seal self-verify、別processでの`mtfs-verify`、`mtfs-unseal`、unsealed payloadとbundleのbyte単位での一致に加え、wrong keyの拒否、ciphertext/tag mutationの拒否、失敗時にpartial plaintextが残らないことまで確認します。

CSPRNGを使用するため、`SENTINEL.MTF`全体のhashは固定しません。

手動でsealする場合は、bundle summaryの`minimum_required_ram_32bit`を`--required-ram`へ指定します。

```console
build/sealed-model/mtfs-seal \
  --key <fleet-key-file> \
  --input <st-root>/sentinel.bundle \
  --output <st-root>/SENTINEL.MTF \
  --target-id 0x53544e36 \
  --accelerator-id 0x4e415254 \
  --model-format 0x534e5431 \
  --required-ram <minimum_required_ram_32bit-from-summary>
```

`mtfs-seal`はpackageごとにmodel key、nonce、model IDをCSPRNGで生成します。

そのため、同じbundleとfleet keyを使用しても、生成される`SENTINEL.MTF`はbyte-identicalにはなりません。

続けて、同じpolicyを指定してpackage全体を検証します。

```console
build/sealed-model/mtfs-verify \
  --key <fleet-key-file> \
  --input <st-root>/SENTINEL.MTF \
  --target-id 0x53544e36 \
  --accelerator-id 0x4e415254 \
  --model-format 0x534e5431
```

Windows native buildでは、実行file名に`.exe`が付きます。

現行Host toolがpackageへ記録するfleet key ID/versionは`1/1`固定です。

target側のactive key recordも、同じID/versionである必要があります。

`apps/key-provision`でfleet keyをreplacementしても、ID/versionは`1/1`のまま維持されます。

ただし、raw key materialは変わるため、old packageをnew keyで認証することはできません。

packageとdevice keyの切り替え順序、切り替え途中のnon-atomicな期間、手動rollbackについては[single-key replacement手順](applications.md#単一fleet-keyの明示的なreplacement)を参照してください。

### 5. SD cardへ配置する

検証済みの`<st-root>/SENTINEL.MTF`を、file名を変更せずSD cardのroot directoryへ配置します。

```text
0:/SENTINEL.MTF
```

targetでは最初に`sentinel-monitor`を実行し、package authentication、runtime policy、32 windowのwarmup、common/media/target diagnosticsを確認してください。

操作方法については[Sentinel Lab manual](applications.md#appssentinel-lab)を参照してください。

## RAとSTで公開artifactが異なる理由

公開範囲は次のとおりです。

| artifact                         | RA / Ethos-U55           | ST / Neural-ART                  |
| -------------------------------- | ------------------------ | -------------------------------- |
| dataset、schema、training/freeze記録 | 公開                       | 公開                               |
| canonical int8 TFLite            | 公開                       | 公開                               |
| numerical/acceptance corpus      | 公開                       | 公開                               |
| target NPU変換recipeと監査記録          | 公開                       | 公開                               |
| NPU向け変換済みartifact                | Vela optimized TFLiteを公開 | Neural-ART generated runtimeを非公開 |
| runtime入りbundle                  | 公開                       | 非公開                              |
| test-key sealed `SENTINEL.MTF`   | 公開                       | 非公開                              |

RAではArm Vela 5.1.0がApache-2.0で提供されており、公開canonical modelから生成したVela TFLite、bundle、sealed test packageについて、由来と公開可否をrepository側で確認しています。

RAの`SENTINEL.MTF`は公開test key専用です。

> **TEST/DEMO KEY - PUBLIC AND NOT SECRET - DO NOT USE IN PRODUCTION**

ST Neural-ART middlewareとgenerated runtimeには、SLA0104および利用者が取得したpackageのlicense条件が適用されます。

repositoryのprovenance記録上、conditional redistributionの条件は確認できるものの、匿名のpublic GitHub配布に必要となるcustomer screening、契約条件のflow-down、redistribution記録などを満たしていることをrepository側で確認できていません。

そのため、`public_commit_decision`は`do_not_commit_st_binaries`としています。

これは、「STでは動作していない」「変換方法が分からない」という意味ではありません。

公開auditには、指定されたtool versionを使用してprivate temporary outputを生成し、acceptance済みruntimeのhashと一致したことを確認した記録があります。

一方で、公開可否を独自に解釈してbinaryを匿名で再配布することはせず、次の境界を設けています。

* project-ownedなdataset、training code、canonical TFLite、numerical corpus、生成scriptは公開する
* ST runtime、runtime入りbundle/`SENTINEL.MTF`、generated firmwareは公開しない
* 利用者は、自身で正規に取得し、適用条件へ同意したST toolchain/packageを使用する
* 利用者環境で生成したartifactの利用条件・再配布条件は、そのdistributionに適用されるlicenseで確認する

判断根拠については[`license-provenance.json`](../artifacts/storage_sentinel/license-provenance.json)および[Third-party software / licenses](licenses.md)に記載しています。

これは法的助言ではありません。

package内では`Package_license`が個別directoryのlicenseより優先される場合があるため、実際に使用するversionのlicense条件を確認してください。

## 最初の確認手順

1. targetのoptimized Release profileをbuildします。
2. targetに対応する`SENTINEL.MTF`をSD rootへ配置します。STでは[STM32N6570-DK用`SENTINEL.MTF`の生成](#stm32n6570-dk用sentinelmtfの生成)に従い、利用者環境で生成します。
3. `sentinel-monitor`を開始し、32 windowの`WARMUP`が完了するまで待ちます。
4. `NORMAL`/`ANOMALY`だけでなく、`source`、score区間、threshold、media generationも確認します。
5. `diag`を実行し、common/media/target error counterが0であることを確認します。
6. 検証時にのみ`pseudo-monitor-delay-ramp`を使用し、natural monitorとはlog/datasetを分離します。
7. `sentinel-infer-profile`でCPU/NPUの数値差、latency、stack、heap delta、runtime diagnosticsを確認します。
8. hotplug testではconsoleに表示される`ACTION REQUIRED`に従い、抜去中のRULE判定と再挿入後のwarmup再実行を確認します。

commandの正確な構文、targetごとの差分、必要なfeature、cleanupについては[Sentinel Lab manual](applications.md#appssentinel-lab)、問題の切り分けについては[Troubleshooting](troubleshooting.md)を参照してください。
