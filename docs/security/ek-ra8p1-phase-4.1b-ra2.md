# EK-RA8P1 Phase 4.1B-RA2 fleet-specific sealed package integration test

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

integration test専用readerは固定した試験profileと期待file sizeを検査し、manifest、envelope、chunkを順次読む。
package全体をRAMへ読み込まず、汎用model catalog、任意metadata/policy、rollback選択、通常modelの
FatFs loaderは実装しない。これらはPhase 4.2のmodel storeに残す。

## Host artifactの生成

WSL2でHost toolをbuildし、実際にprovisioningした32-byte `fleet.key`を指定する。

```sh
cmake -S tools/sealed_model -B /tmp/mtfs-sealed-build -DBUILD_TESTING=ON
cmake --build /tmp/mtfs-sealed-build
ctest --test-dir /tmp/mtfs-sealed-build --output-on-failure

/tmp/mtfs-sealed-build/mtfs-test-package \
  --key /secure/path/fleet.key \
  --package /path/to/sd/MTFSTEST.MTF \
  --descriptor /path/to/sd/MTFSTEST.TXT
```

生成物は次の2ファイルである。SDカードのrootへ両方をコピーする。

- `MTFSTEST.MTF`: fleet固有のsealed package。`K_model`、package ID、nonceは実行ごとにCSPRNG生成する。
- `MTFSTEST.TXT`: package/payload size、公開plaintext pattern、両SHA-256を記録した非秘密descriptor。

`fleet.key`、raw `K_model`、その他の秘密値はどちらにも入らない。descriptorはコピー対象の識別と
Host側監査のための「α」であり、targetのsecurity decisionには使用しない。targetは認証済みpackageを
復号した後、`(offset * 7 + 3) & 0xff`の既知plaintextと直接比較する。

固定公開`fleet_test.key`への入替え、OSPI key version/generationの更新、試験後の鍵復元は不要である。
`stage=fleet-key-mismatch`になった場合は、現在OSPIに入っている鍵と同じ`fleet.key`でpackageを再生成する。

## Console commandと合格条件

```text
crypto-consistency
crypto-package-test
crypto-negative
```

`crypto-consistency`は既存のprovisioned-key GCM round-trip/reimport試験である。`crypto-package-test`は
fleet固有packageのenvelope、model-key wrap、payload既知plaintext照合を検証する。`K_model`、nonce、
package ID、ciphertextは生成ごとに変化するため、固定key/input/expected outputのKATではない。
`crypto-negative`はSD上のfileを変更せず、
読み込んだ暗号文をRAM上だけで改変する。

期待する主要ログ:

```text
[crypto-package-test] source=0:/MTFSTEST.MTF fleet-specific package bytes=5280 chunks=2
[crypto-package-test] envelope PASS model-wrap=PASS raw-k-model-zeroize=PASS
[crypto-package-test] payload chunk=0 bytes=4096 PASS
[crypto-package-test] payload chunk=1 bytes=904 PASS
[crypto-package-test] fleet envelope/wrap/payload PASS scratch-zeroize=PASS ...
[crypto-negative] envelope-tag=REJECT chunk-ciphertext=REJECT chunk-tag=REJECT output-zeroize=PASS PASS ...
```

鍵、raw/wrapped `K_model`、nonce、tag、plaintextはログへ表示しない。FSP 6.5.0が意図的なGCM認証
不一致を`PSA_ERROR_HARDWARE_FAILURE`（`-147`）へ変換するため、同じpackageの正例が成立する経路に
限って`-147`を認証拒否として受理する。

## Build結果

Host CMake buildは`-Wall -Wextra -Wpedantic -Werror`で成功し、CTest 1/1と生成packageの
`mtfs-verify`がPASSした。Arm GCC 13.2.1 Debug buildもwarning/errorなしで成功した。image sizeは
名称整理後のclean buildも成功し、image sizeはtext 321,280 bytes、data 88 bytes、BSS 306,185 bytesである。

EK-RA8P1実機で、generation 1 / key ID 1 / key version 1のOSPI鍵と、同じHost `fleet.key`で生成した
sealed test packageを使用して次をPASSした。

- `crypto-consistency`: empty、partial、4/16/64 KiB、reimport
- `crypto-package-test`: envelope認証・復号、raw `K_model`のInitialKeyWrapとzeroize、4096/904-byte chunkの既知plaintext照合
- `crypto-negative`: envelope tag、chunk ciphertext、chunk tagをすべて`-147`で拒否し、出力zeroize

これにより、Phase 4.1A Host packageからRA8P1のOSPI `K_fleet`、RSIPでwrapped `K_model`、payload chunk
復号までのhardware crypto provider経路をPhase 4.1B-RA2 **PASS**とする。SD上の汎用package選択・policy・
通常model loadは引き続きPhase 4.2の範囲である。

実機PASS後に、固定key/input/outputを使わない試験をKATと呼んでいた名称だけを`mtfs-test-package`、
`MTFSTEST.MTF`、`crypto-package-test`へ変更した。暗号処理と合否条件は変更していないため実機再試験は
要求せず、改称後のHost/RA clean buildとHost package認証を確認した。
