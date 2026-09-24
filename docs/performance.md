# Performance / resource reference

ここに示す値は保証値ではありません。

SD cardのperformanceは、cardの個体差に加え、容量表記だけでは分からないcontroller、wear、温度、空き容量、断片化、内部GCなどの影響を受け、大きく変動します。

また、構成条件の異なる測定値を単純に比較しないでください。

## I/O benchmark contract

`bench-smoke`/`bench-normal`は[非破壊benchmark](storage-operations.md#非破壊performance-benchmark)です。

測定結果を共有または公開する場合は、次の情報をconsole logとあわせて保存してください。

* exact commitとdirty状態、board、Debug/Release、IDE/compiler version
* card manufacturer/model/capacity、FAT種別、cluster size、空き容量
* transport、bus width/SPI clock、cache、IDMA/polling
* request size、total bytes、warm-up回数、run count
* throughput、IOPS、min/average/max latency、checksum/PASS
* common/target error、abort、timeout counter、temporary-file cleanup

## 追跡可能なreference result

次の値は、Git commit `a2bcacd158e7f42205ce41d73dfab39c5106623d`に記録されている2026-08-16時点のRelease実測結果です。

現行HEADで再測定した値ではなく、1 runだけのhistorical referenceとして掲載しています。

request sizeは、各欄とも512 B/4 KiB/32 KiBの順です。

| target / 条件                                                              | raw sequential read KiB/s | FatFs end-sync write / read KiB/s               | FatFs request-sync write / read KiB/s          |
| ------------------------------------------------------------------------ | ------------------------- | ----------------------------------------------- | ---------------------------------------------- |
| EK-RA8P1、SPI 4 MHz、FAT32 4 KiB cluster、cache有効                           | 273.6 / 273.6 / 273.4     | 82.6 / 87.4 / 82.8 ・ 281.4 / 281.4 / 281.2      | 32.7 / 58.2 / 92.4 ・ 277.3 / 275.0 / 275.7     |
| STM32N6570-DK、SDMMC2 4-bit 200 MHz、IDMA+IRQ、FAT32 32 KiB cluster、cache有効 | 826.4 / 4044.0 / 3844.1   | 111.4 / 199.9 / 186.9 ・ 906.6 / 4273.6 / 3651.1 | 31.8 / 184.2 / 179.7 ・ 874.0 / 3934.5 / 3472.7 |
| STM32N6570-DK、同条件polling fallback                                        | 826.0 / 825.3 / 815.3     | 102.8 / 103.5 / 111.4 ・ 911.9 / 911.1 / 857.3   | 50.9 / 35.2 / 121.4 ・ 881.2 / 871.6 / 832.9    |

すべてのcaseでchecksumとtemporary-file cleanupはPASSしています。

RAではSPI/token/ready/clock errorが0、STではHAL error/abort/completion timeout/card-state timeoutが0でした。

ただし、元の測定記録にはcard manufacturer/modelと各caseのlatency全値が残っていません。

そのため、この表の値を現行製品のperformance baselineやcard間比較には使用しないでください。

現行treeのreference値を更新する場合は、同じcardを使用して`bench-info`、`bench-smoke`、`bench-normal`を実行し、加工していないconsole logをreviewへ提出してください。

## Resource usage

次の値は、application/target sourceの最終変更commit `7b1a22bfc89f02083ab047b91160407debb246d3`から生成したRelease ELFを、GNU `size`で確認した結果です。

このcommit以降、現行HEADまでproduction codeの動作に影響する変更はありません。

RAではFSP 6.5.0/Arm GCC 13.2.1、STではCubeIDE 2.1.1/GNU Tools for STM32 14.3.rel1を使用しています。

STの表はAppliのみを対象としており、FSBLのtext/data/BSSである36,212/12/4,052 bytesは含みません。

| application          | EK-RA8P1 text / data / BSS bytes | STM32N6570-DK Appli text / data / BSS bytes |
| -------------------- | -------------------------------: | ------------------------------------------: |
| `apps/simple`        |              40,392 / 0 / 18,952 |                     49,892 / 3,412 / 19,180 |
| target test          |           220,836 / 88 / 466,701 |                   129,156 / 3,412 / 389,352 |
| `apps/key-provision` |            130,652 / 88 / 32,541 |                    33,528 / 3,416 / 147,588 |
| `apps/sentinel-lab`  |          268,028 / 152 / 413,057 |                   168,680 / 4,488 / 370,869 |

feature構成、vendor library、linker layoutが異なるため、これらの値をtarget間やapplication間の効率比較には使用できません。

target testとSentinel Labは16 KiBのcoordinator stackとguardを持ちます。

実行後は`stack-highwater`またはprofile commandを使用し、task stackのhigh-waterを確認してください。

sealed modelは、最大64 KiBのauthenticated plaintext scratchを扱います。

RA Sentinel Labのbundle arenaは32,768 bytes/32-byte alignmentです。公開RA package manifestの`required_ram`は23,728 bytesです。

STのaccepted runtimeでは、required RAMは29,040 bytes、runtime stackは12,288 bytesです。Release実測のstack high-waterは4,072 bytesで、8,216 bytesのmarginが記録されています。

inference commandでは、heap deltaが0であることを要求します。

target固有のNPU runtime、tensor/model arena、DMA bufferについても、各linker mapでmemory regionと相互の重複がないことを確認してください。

## Storage Sentinel reference

RAのpublic packageでは、validation/held-outの各200 vectorを2回ずつ実行し、CPU/NPU間の数値差は0でした。

natural/pseudo monitor、recovery、hotplugについては両targetでacceptance済みです。

ST Releaseでの最終的なNeural-ART application latencyは約78～79 µs、Debugでは129～134 µsでした。CPUとNPUの最終的な数値判定は一致しています。

これらは各target固有のmodel/runtimeに対するacceptance結果であり、NPU同士のperformance比較を目的とした値ではありません。

Debug buildやpolling構成でのclassification qualityは、正式なreferenceとして扱いません。

natural card stallとpseudo delayを混同せず、baseline warmup、OOD、RULE、CPU arbitration、hotplug recoveryを同じsession log上で確認してください。

CP437とCP932のROM使用量の差は、過去に約58～60 KiBと観測されています。

ただし、現行HEADで同一条件による再測定を行っていないため、reference表には掲載していません。
