# STM32N6570-DK SDMMC2 runner

STM32N6570-DK の SDMMC2 4-bit を microT-FS の `pdrv=0` として使う実機 runner です。既定は SDMMC 内蔵 IDMA + SDMMC2 IRQ、代替は polling です。I-cache/D-cache を有効のまま使い、RIF readback、raw read、FatFs roundtrip、2 task 同時アクセス、unmount/remount 後の永続性、IRQ/callback/転送 block 数の診断を実行します。raw sector write と format は行いません。

## 対応ツールとプロジェクト

- STM32CubeIDE 2.1.1
- STM32CubeMX 6.17 系
- STM32Cube FW_N6 V1.3.0
- μT-Kernel BSP2 submodule v1.00.04
- `mtfs_stm32n6570_dk.ioc`
- `FSBL`: First Stage Boot Loader
- `Appli`: secure LRUN application（microT-Kernel と microT-FS はここへリンク）

Cube の HAL/CMSIS/ExtMem ソースは生成プロジェクトの管理対象です。microT-FS、target application、common test、`mtk3_bsp2` は `.project` の相対 linked resource で参照し、コピーを置きません。`Debug/`、`Release/`、workspace metadata、`.elf/.bin/.map` は生成物で Git 管理外です。

`mtk3_bsp2` v1.00.04 の Armv8-M `interrupt.c` には STM32N657 build typo と RAM vector cache coherence の不足があるため、Appli はその 1 ファイルだけ build exclude し、`application/mtfs_stm32n6570_interrupt_override.c` を使います。submodule 本体や RA target は変更しません。

## CubeIDE import / build

1. repository を submodule 込みで checkout し、`git submodule status` が `1ab52cc5a9f59450e62ab78e76de11f4dd89eb15` であることを確認します。
2. CubeIDE の `File > Import > General > Existing Projects into Workspace` で `basic` を root に選び、root、FSBL、Appli を import します。`Copy projects into workspace` は無効にします。
3. `mtfs_stm32n6570_dk_Appli` の Debug、次に `mtfs_stm32n6570_dk_FSBL` の Debug を build します。両方が error/warning 0 であることを確認します。
4. FSBL の Debug Configuration を作り、Startup/Load images で Appli の `Debug/mtfs_stm32n6570_dk_Appli.elf` を download + symbols 対象として追加します。Appli を先に SRAM へ load し、FSBL を load/start する構成にします。

CubeIDE で `.ioc` を再生成すると `Core/Src/main.c`、`stm32n6xx_hal_msp.c`、`stm32n6xx_it.c`、`.project/.cproject` が更新され得ます。再生成前後の diff で次を確認してください。

- USER CODE 内の `HAL_SD_Init` 遅延、pre-kernel RIF、`knl_start_mtkernel()` が残る。
- SDMMC2 global interrupt が enabled、preemption priority 5、subpriority 0。
- SDMMC2 は Appli、4-bit、RIF CID1。
- linked resource、include path、source exclude、3 個の `MTFS_*` define が残り、絶対パスが混入しない。

## IDMA / polling 選択

Appli Debug/Release の C compiler define `MTFS_STM32_SD_USE_IDMA` で切り替えます。

- `1`（既定）: IDMA+IRQ。GPDMA/HPDMA channel は不要で、必要な IRQ は SDMMC2 global interrupt だけです。
- `0`: polling fallback。同じ Block Device/FatFs/test API を使いますが、複数 sector の要求を 1 sector ずつの HAL polling 転送へ分割します。SDMMC hardware flow control もこの経路だけ有効にし、速度より確実性を優先します。IDMA 固有の診断 assertion は省略します。

変更後は clean build してください。実機合格は両設定で別々に確認します。

## RIF、cache、timebase

`mtfs_stm32n6570_dk_pre_kernel_init()` を Cube peripheral setup 後、T-Kernel 起動前に呼びます。SDMMC2 RIMC master index 3 を CID1/Secure/Privileged、SDMMC2 slave を Secure/Privileged に設定し、readback 一致を IDMA の前提条件にします。

IDMA buffer は `mtfs_stm32_sdmmc_context_t` 内の aligned 4096-byte bounce buffer です。Debug map では context が `0x34081480`、buffer が `0x34081520`、linker RAM は `0x34080000` から `0x341fffff` で、32-byte alignment と内部 RAM 配置を満たします。map 値は build ごとに再確認してください。

HAL timeout は microT-Kernel cyclic handler が更新する HAL tick を使います。FatFs は `FF_FS_REENTRANT=1` と microT-Kernel mutex adapter を使用します。lock 順序は FatFs volume mutex が外側、SDMMC port mutex が内側です。

## 実機接続と起動

この runner の Debug は FSBL と Appli を SRAM に load する開発ブート手順です。

1. microSD を取り外し、基板の BOOT switch 2 個を development boot 側（基板正面から右側）へ動かします。
2. ST-LINK USB と VCP terminal（USART1、115200 bps、8 data、no parity、1 stop、flow controlなし）を接続してから電源投入します。
3. CubeIDE の FSBL Debug Configuration から開始し、Appli と FSBL の両 image が download されたことを console で確認します。
4. `BOOT_Application()` 後に Appli の `main()`、microT-Kernel、runner の順で進むことを確認します。

外部 flash 常駐はこの Phase の合否手順ではありません。常駐させる場合は ST signing tool で FSBL/Appli に header を付与し、development boot で NOR external loader `MX66UW1G45G_STM32N6570-DK` を選び、FSBL を `0x70000000`、Appli を `0x70100000` へ program/verify します。その後 BOOT switch 2 個を flash boot 側（左側）へ戻して電源再投入します。未署名の Debug `.bin` をそのまま外部 flash へ書かないでください。

## 実機試験

媒体上の既存ファイルは維持しますが、runner は `mtfs_phase3.bin` と task 別の一時ファイルを作成・検証・削除します。書込み可能な FAT12/16/32 microSD を使用し、必要なデータは事前に backup してください。hot-plug は対象外なので、挿抜のたびに reset します。

1. カード未挿入で smoke profile（`MTFS_STM32N6570_TEST_PROFILE=1`）を起動し、SD init が有限時間で FAIL して system が hang しないことを確認します。
2. カードを挿入して reset し、IDMA=1 / smoke で `cache I=enabled D=enabled`、`RIF ready=1`、geometry、raw read PASS、各 test PASS、最終 `PHASE 3 RUN PASS` を確認します。
3. 診断値で IRQ/Rx/Tx が 1 以上、error/timeout が 0、read single/multi と write multi が 1 以上、read/write max が 2 以上であることを確認します。
4. normal profile（既定、10 rounds）、stress profile（`MTFS_STM32N6570_TEST_PROFILE=3`、100 rounds）を実行し、全 round PASS、mount/unmount、2 task 同時 read/write、remount 後 compare が継続することを確認します。
5. `MTFS_STM32_SD_USE_IDMA=0` へ切り替えて clean build し、カード未挿入 smoke、カード挿入 smoke/normal/stress を同様に実行します。IRQ 診断 assertion がないこと、read/write の `multi=0` と `max=1`、filesystem の結果が IDMA と同じことを確認します。
6. 各構成で複数回 power-cycle し、再起動後も mount と test が成功することを確認します。

IDMA 経路を debugger でも追う場合は、`mtfs_stm32_sd_irq_handler()`、`HAL_SD_RxCpltCallback()`、`HAL_SD_TxCpltCallback()`、`HAL_SD_ErrorCallback()` に breakpoint を置きます。正常時は Rx/Tx に到達し、Error には到達しません。次も watch してください。

- `sd_context.diagnostics`: IRQ/Rx/Tx と single/multi/max、error/timeout/abort
- `sd_context.bounce_buffer`: アドレス下位 5 bit が 0、領域が `0x34080000..0x341fffff`
- `RIFSC->RIMC_ATTRx[3]`: MCID=1、MSEC=1、MPRIV=1
- `RIFSC->RISC_SECCFGRx[1]` / `RISC_PRIVCFGRx[1]`: bit 22 が 1
- `SCB->CCR`: `IC` と `DC` bit がともに 1
- `uwTick`: T-Kernel 起動後も増加する

timeout/abort 復旧を意図的に試す場合は媒体を取り外した状態でのみ行い、挿入済み媒体への転送中に breakpoint を長時間止めないでください。停止時間が 5 秒を超えると timeout が期待どおり発生します。

Phase 3 完了条件は、両転送経路で上記実機試験がすべて成功し、host regression と CubeIDE build が成功することです。実機ログなしに Phase 3 完了とは判定しません。
