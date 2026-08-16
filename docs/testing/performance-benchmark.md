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

Phase 3.4a以降は各benchmark commandの終了時にRA SPI wait診断も表示します。正常なbaselineでは`token_timeouts=0`、`ready_timeouts=0`、`monotonic_clock_errors=0`を確認し、token/readyの`calls`、`polls`、`max`と`acmd41_retries`もconsole logへ残してください。

計測時計はmicroT-Kernelの64-bit monotonic operating timeとSysTickの現在値を組み合わせます。kernel tickは10 msですが、timestamp分解能はcore cycle相当です。RTCやwall-clock変更の影響を受けず、長いI/Oの経過時間は64-bit operating timeが保持するため32-bit cycle counter wrapには依存しません。

## STM32N6570-DKでの実行

CubeIDEで`tests/targets/stm32n6570_dk/basic/Appli`のReleaseと対応するFSBLをbuildして書き込みます。既存basic test完了後のcommand consoleで、RAと同じ順に`bench-info`、`bench-smoke`、`bench-normal`を実行します。

既定の`MTFS_STM32_SD_USE_IDMA=1`はSDMMC2 4-bit IDMA+IRQで、複数block要求をHAL DMAへ保持します。比較用のpolling baselineはcompiler defineを0へ変更してclean buildし、同じカードと手順で測定します。polling portは複数sector要求を単一sector HAL callへ分割します。`bench-info`にはHALが初期化後に設定したCLKCRとSDMMC2 source clockから算出した実bus clockを出力します。

各commandの終了時にSDMMC診断も表示します。正常なbaselineではHAL error callback、abort、completion timeout、card-state timeoutがすべて0であること、IDMAではIRQ/Rx/Tx callbackとmulti-block counterが増えることを確認してください。

比較用にconsole logを加工せず保存し、次も一緒に記録してください。

- SDカードのメーカー、型番、容量。同じカードをSTでも使ったか
- git commit、dirty状態、FSP/toolchain、Debug/Release、追加compile definition
- 電源投入からcommandまでの手順とcommand実行回数
- `error`、timeout、card removal、pattern mismatch、cleanup failureの有無

## Baseline

Phase 3.4の測定はPhase 3.4aのRA SPI wait/timeout修正の間だけ中断しました。修正前Debug buildでは、EK-RA8P1 / SPI 4 MHzのraw 4 KiB request、1 MiB readが122,873,697 us、8.3 KiB/s、2.0 IOPS、平均latency 479,904 usでPASSしました。これはpollごとの10 ms delayを含む原因確認用referenceであり、Release baselineには採用しません。

EK-RA8P1の実測baselineは2026-08-16にPhase 3.4a修正済みRelease (`build=optimized`) で取得しました。SDHC/SDXC、FAT32、4 KiB cluster、SPI 4 MHz、cache有効、warm-upなし、各case 1 runです。throughputはrequest size 512 B / 4 KiB / 32 KiBの順に記載します。

STM32N6570-DKのIDMA+IRQ baselineも2026-08-16にRelease (`build=optimized`) で取得しました。SDHC/SDXC、FAT32、SDMMC2 4-bit 200 MHz、32 KiB cluster、cache有効、warm-upなし、各case 1 runです。IDMAは複数block要求を保持し、実測counterのread/write最大転送は8 blockでした。

同日のpolling baselineは同じRelease条件で`MTFS_STM32_SD_USE_IDMA=0`へ切り替えて取得しました。portが全要求を1 sector HAL callへ分割するため、IRQ/Rx/Tx callbackとmulti-block counterは0、read/write最大転送は1 block、SDMMC hardware flow controlは有効でした。

| target/path | raw read | FatFs write/read | request-sync | error/abort/timeout | verification/cleanup |
|---|---|---|---|---|---|
| EK-RA8P1 / SPI 4 MHz | 273.6 / 273.6 / 273.4 KiB/s | end-sync write 82.6 / 87.4 / 82.8 KiB/s; read 281.4 / 281.4 / 281.2 KiB/s | write 32.7 / 58.2 / 92.4 KiB/s; read 277.3 / 275.0 / 275.7 KiB/s | SPI 0; token timeout 0; ready timeout 0; clock error 0 | all cases PASS; checksum PASS; cleanup 6/6 |
| STM32N6570-DK / polling | 826.0 / 825.3 / 815.3 KiB/s | end-sync write 102.8 / 103.5 / 111.4 KiB/s; read 911.9 / 911.1 / 857.3 KiB/s | write 50.9 / 35.2 / 121.4 KiB/s; read 881.2 / 871.6 / 832.9 KiB/s | HAL error 0; abort 0; completion/card-state timeout 0/0 | all cases PASS; checksum PASS; cleanup 6/6; multi-block 0/max 1 |
| STM32N6570-DK / IDMA + IRQ | 826.4 / 4044.0 / 3844.1 KiB/s | end-sync write 111.4 / 199.9 / 186.9 KiB/s; read 906.6 / 4273.6 / 3651.1 KiB/s | write 31.8 / 184.2 / 179.7 KiB/s; read 874.0 / 3934.5 / 3472.7 KiB/s | HAL error 0; abort 0; completion/card-state timeout 0/0 | all cases PASS; checksum PASS; cleanup 6/6; multi-block max 8 |

### Phase 3.5 diagnostics enabledでのRA退行確認

2026-08-16にEK-RA8P1 Release、SPI 4 MHz、diagnostics enabledで`bench-smoke`と`bench-normal`を実機実行し、全case、pattern検証、cleanupがPASSしました。`bench-normal`のraw readは512 B / 4 KiB / 32 KiB requestで273.8 / 273.7 / 273.5 KiB/sでした。Phase 3.4 baselineの273.6 / 273.6 / 273.4 KiB/sとの差は+0.2 / +0.1 / +0.1 KiB/sで、代表値の4 KiBは約+0.04%です。1 run同士の比較ですが、少なくともdiagnostics追加による重大なread性能退行は観測されていません。

同じrunの診断ではcommon read/write completed sectorが12,838 / 6,866、RA typed read/write sectorも12,838 / 6,866で一致しました。SPI transfer starts/completionsは11,616,428で一致し、transfer error、token/ready timeout、monotonic clock error、共通read/write/sync failureはすべて0でした。
