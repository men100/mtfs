# EK-RA8P1 Phase 4.1B-RA2 fleet-specific package KAT

状態: **HARDWARE PASS**（2026-08-20）。

## 目的と境界

Phase 4.1B-RAで実機PASSしたOSPI上のHUK-wrapped `K_fleet`を変更せず、Hostに保管した同じ
`fleet.key`でPhase 4.1A形式の試験packageを生成し、次の実経路を検証する。

1. SDからmanifestと48-byte `K_model` envelopeを読む。
2. OSPIのwrapped `K_fleet`をvolatile PSA handleへimportする。
3. manifestをAADとしてenvelopeを認証・復号する。
4. 得られた32-byte raw `K_model`を直ちに`R_RSIP_AES256_InitialKeyWrap()`へ渡す。
5. raw `K_model`をzeroizeし、HUK-wrapped `K_model`をvolatile PSA handleへimportする。
6. 4096-byte chunkと904-byte chunkを認証・復号し、既知の5000-byte plaintextと比較する。
7. envelope tag、先頭chunk ciphertext、末尾chunk tagのRAM上の1-bit改変を拒否し、出力zeroizeを確認する。

KAT専用readerは固定した試験profileと期待file sizeを検査し、manifest、envelope、chunkを順次読む。
package全体をRAMへ読み込まず、汎用model catalog、任意metadata/policy、rollback選択、通常modelの
FatFs loaderは実装しない。これらはPhase 4.2のmodel storeに残す。

## Host artifactの生成

WSL2でHost toolをbuildし、実際にprovisioningした32-byte `fleet.key`を指定する。

```sh
cmake -S tools/sealed_model -B /tmp/mtfs-sealed-build -DBUILD_TESTING=ON
cmake --build /tmp/mtfs-sealed-build
ctest --test-dir /tmp/mtfs-sealed-build --output-on-failure

/tmp/mtfs-sealed-build/mtfs-kat-package \
  --key /secure/path/fleet.key \
  --package /path/to/sd/MTFSKAT.MTF \
  --descriptor /path/to/sd/MTFSKAT.TXT
```

生成物は次の2ファイルである。SDカードのrootへ両方をコピーする。

- `MTFSKAT.MTF`: fleet固有のsealed package。`K_model`、package ID、nonceは実行ごとにCSPRNG生成する。
- `MTFSKAT.TXT`: package/payload size、公開plaintext pattern、両SHA-256を記録した非秘密descriptor。

`fleet.key`、raw `K_model`、その他の秘密値はどちらにも入らない。descriptorはコピー対象の識別と
Host側監査のための「α」であり、targetのsecurity decisionには使用しない。targetは認証済みpackageを
復号した後、`(offset * 7 + 3) & 0xff`の既知plaintextと直接比較する。

固定公開`fleet_test.key`への入替え、OSPI key version/generationの更新、試験後の鍵復元は不要である。
`stage=fleet-key-mismatch`になった場合は、現在OSPIに入っている鍵と同じ`fleet.key`でpackageを再生成する。

## Console commandと合格条件

```text
crypto-consistency
crypto-kat
crypto-negative
```

`crypto-consistency`は既存のprovisioned-key GCM round-trip/reimport試験である。`crypto-kat`はfleet固有
packageのenvelope、model-key wrap、payload既知解を検証する。`crypto-negative`はSD上のfileを変更せず、
読み込んだ暗号文をRAM上だけで改変する。

期待する主要ログ:

```text
[crypto-kat] source=0:/MTFSKAT.MTF fleet-specific package bytes=5280 chunks=2
[crypto-kat] envelope PASS model-wrap=PASS raw-k-model-zeroize=PASS
[crypto-kat] payload chunk=0 bytes=4096 PASS
[crypto-kat] payload chunk=1 bytes=904 PASS
[crypto-kat] fleet envelope/wrap/payload PASS scratch-zeroize=PASS ...
[crypto-negative] envelope-tag=REJECT chunk-ciphertext=REJECT chunk-tag=REJECT output-zeroize=PASS PASS ...
```

鍵、raw/wrapped `K_model`、nonce、tag、plaintextはログへ表示しない。FSP 6.5.0が意図的なGCM認証
不一致を`PSA_ERROR_HARDWARE_FAILURE`（`-147`）へ変換するため、同じpackageの正例が成立する経路に
限って`-147`を認証拒否として受理する。

## Build結果

Host CMake buildは`-Wall -Wextra -Wpedantic -Werror`で成功し、CTest 1/1と生成packageの
`mtfs-verify`がPASSした。Arm GCC 13.2.1 Debug buildもwarning/errorなしで成功した。image sizeは
text 321,164 bytes、data 88 bytes、BSS 306,185 bytesである。

EK-RA8P1実機で、generation 1 / key ID 1 / key version 1のOSPI鍵と、同じHost `fleet.key`で生成した
`MTFSKAT.MTF`を使用して次をPASSした。

- `crypto-consistency`: empty、partial、4/16/64 KiB、reimport
- `crypto-kat`: envelope認証・復号、raw `K_model`のInitialKeyWrapとzeroize、4096/904-byte chunkの既知解
- `crypto-negative`: envelope tag、chunk ciphertext、chunk tagをすべて`-147`で拒否し、出力zeroize

これにより、Phase 4.1A Host packageからRA8P1のOSPI `K_fleet`、RSIPでwrapped `K_model`、payload chunk
復号までのhardware crypto provider経路をPhase 4.1B-RA2 **PASS**とする。SD上の汎用package選択・policy・
通常model loadは引き続きPhase 4.2の範囲である。
