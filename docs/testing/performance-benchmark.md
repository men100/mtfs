# Performance benchmark

Phase 3.4のbenchmarkはmicroT-FSを最適化するものではなく、LFN導入前の現在のI/O特性を、カード、bus、clock、build設定を含む条件付きbaselineとして記録するためのものです。Hostの値はロジック検証専用であり、実機性能の比較には使いません。

## 非破壊方針

- raw block試験はread-onlyです。sector 0からgeometry末尾までsequentialに進み、安全にsector 0へwrapします。raw write、format、trimは呼びません。
- FatFs試験が所有するファイルは8.3名の `MTFSBEN.TMP` だけです。開始時に同名ファイルが存在すれば `stage=temp_file_exists` で中止し、上書きも削除もしません。
- `f_getfree()` で必要量を確認してから `FA_CREATE_NEW` で作成します。作成に成功した場合だけ、成功・失敗の両経路でcloseとunlinkを試みます。
- 動的メモリは使いません。target runnerが32 KiBの固定bufferを渡し、production libraryへbenchmark用RAM/ROMやAPIを追加しません。
- mount/unmountは計測時間に含めず、各suiteの最後にunmountします。SDカードを自動formatしません。

## Profile

全targetで同じprofileとrequest sizeを使います。1 commandを1 runとし、benchmark自身はwarm-upを行いません。

| profile | raw read（request sizeごと） | FatFs end-sync（request sizeごと） | FatFs request-sync（request sizeごと） |
|---|---:|---:|---:|
| smoke | 64 KiB | 64 KiB write + readback | 32 KiB write + readback |
| normal | 1 MiB | 1 MiB write + readback | 64 KiB write + readback |

各欄を512 B、4 KiB、32 KiB requestで実行します。end-syncは全data write後に `f_sync()` を1回行い、request-syncは各 `f_write()` の直後と最後に `f_sync()` を行います。readはwrite close後に再openし、全byteを決定的patternと照合します。

## 指標とログ

ログは `[BENCH]` prefix付きkey=value形式です。

- `elapsed_us`: 対象data phaseの経過時間。mount/unmountは除外します。
- `throughput_kib_s`: `bytes / elapsed` のKiB/s。浮動小数点printfを使わず小数1桁を整数演算で出します。
- `iops`: applicationが発行したblock readまたはFatFs read/write request数を基にした毎秒request数です。port内部でのsector分割数ではありません。
- `latency_us`: 各block readまたは各 `f_read()` / `f_write()` callのmin/average/maxです。
- `final_sync_us`: data write後の最後の `f_sync()` だけの時間です。
- `total_close_us`: 最初のdata writeからfinal syncとclose完了までの時間です。
- `checksum`: raw/read dataが実際に消費されたことを示すFNV-1a値です。FatFs readはchecksumに加えpattern一致を合否判定にします。

SDカードの型番・状態、filesystemの断片化、FAT種別、cluster size、空き容量、bus clock、cache、compiler最適化は結果へ影響します。RA SPIとST SDMMCの値を単純なMCU性能差として扱わないでください。

## EK-RA8P1での実行

e² studioで `tests/targets/ek_ra8p1/basic` のReleaseをbuildして書き込みます。起動時の既存basic testが完了し、console promptが出た後に次を入力します。

```text
bench-info
bench-smoke
bench-normal
```

まず `bench-smoke` の全case、pattern検証、6回の `cleanup file_removed=yes`、`SUITE END status=PASS` を確認してから `bench-normal` を実行してください。RAはSCI_B SPI 4 MHz、1-bit、blocking IRQ completionです。複数sector requestもport内部でCMD17/CMD24のsingle-sector反復になるため、その条件をtarget logへ出します。

計測時計はmicroT-Kernelの64-bit monotonic operating timeとSysTickの現在値を組み合わせます。kernel tickは10 msですが、timestamp分解能はcore cycle相当です。RTCやwall-clock変更の影響を受けず、長いI/Oの経過時間は64-bit operating timeが保持するため32-bit cycle counter wrapには依存しません。

比較用にconsole logを加工せず保存し、次も一緒に記録してください。

- SDカードのメーカー、型番、容量。同じカードをSTでも使ったか
- git commit、dirty状態、FSP/toolchain、Debug/Release、追加compile definition
- 電源投入からcommandまでの手順とcommand実行回数
- `error`、timeout、card removal、pattern mismatch、cleanup failureの有無

## Baseline

実測値はまだ未記入です。測定後、同じカードとnormal profileのログから次を埋めます。

| target/path | raw read | FatFs write/read | request-sync | error/abort/timeout | verification/cleanup |
|---|---|---|---|---|---|
| EK-RA8P1 / SPI 4 MHz | pending | pending | pending | pending | pending |
| STM32N6570-DK / polling | pending | pending | pending | pending | pending |
| STM32N6570-DK / IDMA + IRQ | pending | pending | pending | pending | pending |

