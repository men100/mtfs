# ADR 0002: RA8P1 T-Monitor受信待ちの互換修正

- Status: Accepted; hardware validation pending
- Date: 2026-08-15
- Target: EK-RA8P1, mtk3_bsp2 v1.00.04 (`1ab52cc`)
- Upstream fix: `6e18f885347a485b47448d8fbb46c3a0b028431d`

## Context

BSP2 v1.00.04のEK-RA8P1 `tm_rcv_dat()`は、SCI8 `CSR.RDRF`が0で通信errorも
ない場合に、受信先へ書き込まず`size`だけを減らしてreturnする。呼出元の
`tm_getchar()`は未初期化のlocal byteを返すため、RTC consoleへ入力していなくても
偶然のprintable byteが連続入力される。実機では`0x22` (`"`)が繰り返され、line
buffer上限で`ERROR: input line too long`が継続した。

TRON Forumのdevelop branchでは2026-07-03のcommit
[`6e18f885`](https://github.com/tron-forum/mtk3_bsp2/commit/6e18f885347a485b47448d8fbb46c3a0b028431d)
により、RA8P1/D1/M1の受信処理が`RDRF`まで待つよう修正されている。

## Decision

submoduleはv1.00.04のまま変更しない。EK-RA8P1 targetのlinkerへ
`--wrap=tm_rcv_dat`を追加し、`application/mtfs_ra8p1_tm_com_patch.c`で次を行う。

- `size`はbyteを受信した場合だけ減らす。
- `RDRF`が0ならpollingを継続する。
- framing/parity/overrun errorは`CFCLR`でclearして受信を継続する。
- buffer pointerは受信byteごとに一度だけ進め、複数byteにも対応する。

submoduleへ直接適用する場合の最小差分は
`tests/targets/ek_ra8p1/basic/patches/mtk3_bsp2-v1.00.04-ra8p1-tm-rcv-dat.patch`
に保存する。上流sourceがCRLFのため、確認時は
`git apply --check --ignore-space-change <patch>`を使用する。

## Consequences and removal

修正後は`tm_getchar(1)`が実際の受信byteまでblockingする。これはlibtmの
「pollingは非対応、wait指定だけをsupport」という既存契約に一致する。

submoduleを上流commit `6e18f885`以降へ更新し、RA8P1 `tm_rcv_dat()`の受信待ちを
確認した時点で、次をまとめて削除する。

- `application/mtfs_ra8p1_tm_com_patch.c`
- Debug/Release linkerの`--wrap=tm_rcv_dat`
- 保存したv1.00.04向けpatch

## Validation status

Debug/Releaseのcompile/linkが成功した。両ELFのsymbolとdisassemblyで、元の
`tm_rcv_dat`がlinkされず、`tm_getchar()`が`__wrap_tm_rcv_dat`を直接callすることを
確認した。

残る実機確認は、console開始後に無入力文字が出ないことと、`status`、`get`、
`test-fatfs-time`が正常に入力できることである。
