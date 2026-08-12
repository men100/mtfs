# ADR 0001: RA8P1 RAM vector tableのcache coherency

- Status: Accepted and hardware-validated for Phase 2.1 normal profile
- Date: 2026-08-12
- Target: EK-RA8P1 CPU0, Cortex-M85, RA FSP 6.5.0, mtk3_bsp2 v1.00.04 (`1ab52cc`)

## Context

Phase 2の実機では、I-cache/D-cache有効時に初期task開始前の例外処理から
PCが`0xEFFFFFFE`付近へ進みlockupした一方、`hal_entry()`で両cacheを無効に
するとSD/FatFs試験が完走した。当時保存された実機情報はこのPCとcache無効化
による差分までで、VTOR、例外番号、CFSR/HFSR、fault stackは保存されていない。
したがってレジスタ値を後から推定値で補わない。

コード上の更新順序は次のとおりである。

1. FSP `SystemInit()` がFlashの`__Vectors`をVTORへ設定する。
2. FSPはC runtime、IRQ設定、BSP初期化後にMPUを設定し、設定
   `BSP_CFG_DCACHE_ENABLED=1`によりD-cacheを有効化する。I-cacheもFSPの
   通常設定で有効である。
3. `hal_entry()`からBSP2 `knl_start_mtkernel()`へ入る。割り込み禁止後、
   現VTORを`knl_exctbl_o`へ保存し、`N_SYSVEC + N_INTVEC`（16 + 96）entryを
   `.mtk_exctbl`の`knl_exctbl`へコピーする。
4. BSP2 v1.00.04はD-cache cleanもbarrierも行わずVTORをRAMへ変更する。
5. `knl_main()`から`knl_init_interrupt()`がsystem exception entryを更新する。
6. 起動後もsystem call `tk_def_int()`から`knl_define_inthdr()`を通り、外部
   interrupt entryを登録・解除できる。

つまりvector更新は起動時だけではない。3つの書込み経路すべてにmaintenanceが
なく、cacheable RAMのdirty lineとexception vector fetchの可視性が保証されない。
FSPがFlash vectorを構成する処理自体ではなく、動的RAM vectorを所有するBSP2側の
契約不足である。

Phase 2 ELFではFlash vectorが`0x02000000`、RAM vectorが`0x2200BE00`だった。
Phase 2.1のlink結果では配置は`0x2200C000`へ変化したため、固定アドレスには依存
しない。BSP2の配列宣言は必要entry数の4倍を確保しているが、実際にコピー・参照
する範囲は112 entry、448 byteである。この過剰確保は別の上流課題であり、今回の
maintenanceは実際に使用する448 byteだけを対象にする。

## Decision

submoduleを変更せず、EK-RA8P1 test targetのlinker `--wrap`で次を行う。

- `knl_start_mtkernel`: コピー後、使用範囲をcache line境界へ外向きに丸めて
  `SCB_CleanDCache_by_Addr()`でcleanし、DSB、VTOR更新、DSB、ISBの順にする。
- `knl_init_interrupt`: system exception更新とPhase 2.1 fault handler登録後、
  使用範囲全体をcleanしてからDSB/ISBする。
- `knl_define_inthdr`: 登録または解除した1 entryを含むcache lineだけをcleanし、
  DSB/ISBする。これにより起動後の`tk_def_int()`も対象になる。

line sizeは`SCB->CTR.DMINLINE`から実行時に取得する。RA8P1では32 byteであり、
CMSIS APIへ渡す開始・終了を明示的に32 byte境界へ丸める。D-cacheが無効なら
clean APIは呼ばない。dirty lineをinvalidateして更新内容を失う処理は行わない。

I-cache invalidateは行わない。更新対象はhandler codeではなく、exception entry
としてdata accessされる関数ポインタだけであり、handler code自身は変更しない
ためである。VTOR更新後のISBは新しい制御状態を後続命令に反映するため必要である。

通常buildの`MTFS_RA8P1_DISABLE_CACHES_FALLBACK`は0である。診断のため1を定義した
場合だけ旧来のI/D cache全面無効化を有効にする。起動ログはI/D cache状態、VTOR、
RAM vector範囲、line size、clean回数、fallback状態を表示し、cache無効なら試験を
FAIL扱いにする。

上流へ移植できる最小案は
`tests/targets/ek_ra8p1/basic/patches/mtk3_bsp2-v1.00.04-vector-cache-coherency.patch`
に保存する。submoduleには適用しない。上流sourceがCRLFのため、確認・適用時は
`git apply --ignore-space-change <patch>`を用いる。

## Fault diagnostics

wrapped `knl_init_interrupt()`はHardFault、MemManage、BusFault、UsageFaultを共通の
test-only handlerへ向ける。handlerは`g_mtfs_ra8p1_fault_snapshot`へ次を保存して
BKPTで停止する。

- exception number、CFSR、HFSR、MMFAR、BFAR、SHCSR、VTOR
- MSP、PSP、EXC_RETURN
- stacked LR、PC、xPSR（extended FP frameも考慮）

debuggerでは`valid == 0x4D544653`を保存完了条件として構造体を確認する。fault
entry自身へ到達できないlockupではCPUをhaltし、同じSCB register、IPSR、MSP、PSP、
LR、VTORと、EXC_RETURNが示すstackのPCを直接確認する。

## Consequences and removal

target workaroundはBSP2 startupの一部を複製するため、BSP2更新時に差分確認が必要
である。BSP2が3経路すべてに同等のclean/barrierを備えたら、次をまとめて削除する。

- `mtfs_ra8p1_vector_cache.c/.h`
- `.cproject`の3つの`--wrap`
- fallback定義とtarget固有diagnostic log

上流修正版採用後もfault snapshotは必要なら独立したtest utilityとして残せる。

## Validation status

2026-08-12にDebug ELFのcompile/link、wrapされたstartup/init、mutex adapterの
linkを確認した。同日、EK-RA8P1実機でnormal profile 10周を実行し、
I-cache/D-cacheが有効、全面無効化fallbackがoffのまま全周完走した。起動時の
観測値はVTOR `0x2200c000`、RAM vector範囲
`[0x2200c000, 0x2200c1c0)`、使用サイズ448 byte、D-cache line 32 byte、
clean回数2だった。

各周でSDHC/SDXCのgeometry/sector確認、FatFs roundtrip（14 checks）、
FatFs/microT-Kernel並行テスト（21 checks）が失敗0でPASSし、最終結果は
`[mtfs] PHASE 2.1 PASS`だった。Host側もCTestが1/1 PASS、並行テストが
62 checksでPASSした。

stress profile 100周は実施していない。今回はnormal 10周とhost試験を
Phase 2.1の受入条件とし、stressは完了判定に含めない。
