# EK-RA8P1 RSIP-E50D ハードウェア暗号スパイク（Phase 4.1B-RA）

状態: **GENERATED-KEY GCM HARDWARE PASS / FIXED-KEY PROVISIONING BLOCKED /
NOT PRODUCTION-READY**（2026-08-19）。

本レポートは、`tests/targets/ek_ra8p1/basic` で使用している RA FSP 6.5.0 の
実際のパックを対象に実施した、Phase 4.1B-RA の事前調査結果を記録するものである。
generated-key GCM経路は実機PASSしたが、fixed fleet key経路を含むハードウェアスパイク
全体の完了を示すものではない。ターゲットの NVM、OTP、
オプション設定メモリ、ライフサイクル状態、デバッグロック設定への書込みは
一切実施していない。

## 初回preflightで受入れ試験を阻害した事項

Phase 4.1A の固定フリート鍵をプロビジョニングし、完全な電源断を挟んだ後に
再利用することを妨げた、相互に独立した二つの初回ブロッカーは次のとおりだった。

1. 選択されているデバイスは `R7KA8P1KFLCAC` である。生成された
   `bsp_mcu_device_pn_cfg.h` では `BSP_DATA_FLASH_SIZE_BYTES` が `0` と定義され、
   現在のリンカーメモリマップでも `DATA_FLASH` の長さはゼロである。
   このプロジェクトが管理するその他の不揮発領域は、予約も文書化もされていない。
   RTC バックアップレジスタは RTC の有効性確認にすでに使用されており、
   ラップ済み鍵の格納領域ではなく、完全な電源断をまたいだ保持も保証されない。
   したがって、アドレスを推測することはせず、書込み・消去の実装も追加していない。
2. インストール済みの FSP 6.5.0 パックでは、
   `R_RSIP_AES256_InitialKeyWrap()` は `r_rsip_key_injection` に属している。
   その実装は、RSIP-E50D Compatibility/Plaintext コンポーネントが提供する
   `HW_SCE_GenerateOemKeyIndexSub()` を呼び出す。コンフィグレータでは、
   key injection はこの plaintext コンポーネントのオプション要件として公開されている。
   このコンポーネントと `r_rsip_e50d_protected` は、どちらも単一の
   `interface.peripheral.rsip` を提供する。Protected Mode 自体には
   `R_RSIP_KeyGenerate()` と暗号化鍵/KUK のインポート経路があるが、
   raw AES-256 鍵をインポートするためのサポート済み API はない。
   選択された plaintext プリミティブのファイルを Protected Mode の生成ソースへ
   混在させることは、FSP がサポートしない変更になるため実施していない。

保存先については、内部MRAMを通常版が所有する設計を採用せず、RFP注入済みの
HUK-wrapped blobを専用appでSDへcopyするcontest profileを選択した。詳細は
[`ek-ra8p1-sd-key-provisioning.md`](ek-ra8p1-sd-key-provisioning.md)に記録する。
これにより通常版のMRAM予約は不要になるが、RA8P1 factory boot interfaceによるinitial
AES-256 `.rkey`注入可否は、RFP/SKMT導入後のGo/No-Go gateとして残る。

このため、Host の golden vector に含まれるエンベロープの復号、直後の `K_model`
インポート、ラップ済み鍵の永続化、再起動／完全電源断サイクルの検証、固定鍵を用いた
サイズ別試験／ベンチマーク、および SD/FatFs のエンドツーエンド検証も阻害されている。
これらの項目は PASS ではなく、引き続き NOT RUN としなければならない。

## ターゲット試験に残した安全な実装

`configuration.xml` では、FSP 6.5.0 の RSIP-E50D Protected Mode スタックを
`g_rsip` として選択している。有効にしているのは AES-256、AES-GCM、SHA-256 のみで、
未使用のアルゴリズムは無効化している。Flash モジュールは選択していない。

ターゲット専用の `mtfs_ra8p1_crypto_spike.c` ハーネスは、
`MTFS_RA8P1_CRYPTO_SPIKE_ENABLE=1` が明示的に指定された場合にのみコンパイルされる。
チェックインされている既定値はゼロであるため、通常の Debug/Release アプリケーションでは
コマンドを公開せず、64 KiB のスクラッチバッファも確保しない。このハーネスには、
フリート試験鍵のバイト列も Phase 4.1A ベクターのバイト列も含まれていない。

オプションを有効にした場合、`crypto-info` は、選択されているモード、
アプリケーション側で 16 バイト境界にアラインされた 52 バイトの AES-256 ラップ済み鍵値、
64 KiB の静的スクラッチ領域、診断情報、および NVM／プロビジョニングのブロッカーを表示する。
鍵素材、nonce、tag、plaintext は出力しない。

`crypto-kat` と `crypto-negative` は、限定的な Protected Mode スモーク試験を実行する。

- `R_RSIP_Open()` と `R_RSIP_Close()`
- ハードウェア生成の AES-256 ラップ済み鍵を作成する `R_RSIP_KeyGenerate()`
- 分割 AAD、4 KiB 単位の複数回 Update、16 バイト tag、空のテキスト、
  37 バイトの端数最終ブロック、および 4/16/64 KiB のテキストを使用した GCM 暗号化／復号
- 改変した tag の拒否
- private scratch への復号と、検証成功後に限定したコピー
- 末尾の sentinel を保持しつつ、出力先の試行対象 prefix のみを正確にゼロ化
- Close/Open による復旧と、RAM 内ラップ済み鍵の再利用

この試験は、固定鍵による既知解試験ではなく、生成鍵によるラウンドトリップとして
意図的に表示される。初回の実機試行では、ハーネスが GCM 経路から CCM 専用の
`R_RSIP_AES_AEAD_LengthsSet()` を呼び出していたため、`crypto-kat` と
`crypto-negative` が FAIL した。この呼出しを除去した修正版を実機で再試験し、PASSした。
この初回結果を RSIP の GCM 処理自体の不合格とは判定しない。

状態を持つコマンド、または前提条件に依存するコマンド
（`crypto-provision-test-key`、`crypto-reboot-check`、
`crypto-powercycle-check`、`crypto-clear-test-key`、`crypto-golden`、
`crypto-bench`、`crypto-sd`、`crypto-run`）はすべて BLOCKED と
`ACTION REQUIRED` 行を表示する。書込み、消去、フォーマット、SD ファイルの変更は行わない。

## 確認した FSP 6.5.0 の正確な API と動作

オンライン情報から名前を推測するのではなく、インストール済みのヘッダーとソースを確認し、
次のインターフェースを特定した。

- `R_RSIP_Open(rsip_ctrl_t *, const rsip_cfg_t *)`
- `R_RSIP_Close(rsip_ctrl_t *)`
- `R_RSIP_KeyGenerate(rsip_ctrl_t *, rsip_wrapped_key_t *)`
- `R_RSIP_AES_AEAD_Init(..., RSIP_AES_AEAD_MODE_GCM_ENC/DEC, ..., nonce, nonce_length)`
- `R_RSIP_AES_AEAD_LengthsSet(..., aad_length, text_length, tag_length)`
- `R_RSIP_AES_AEAD_AADUpdate(...)`
- `R_RSIP_AES_AEAD_Update(...)`
- `R_RSIP_AES_AEAD_Finish(...)`
- `R_RSIP_AES_AEAD_Verify(...)`
- compatibility 側の `R_RSIP_AES256_InitialKeyWrap(...)`

`rsip_wrapped_key_t` は、型と、呼出し元が所有する値へのポインターで構成される。
AES-256 では、plaintext 8 ワードに設定済みのラップ済み鍵オーバーヘッド 5 ワードを加えた、
合計 13 ワード（52 バイト）を使用する。ハーネスでは、すべての暗号バッファについて、
最低要件であるワード境界より厳しい 16 バイト境界のアラインメントを適用している。

GCM の実装は、端数の最終ブロックをバッファリングする。復号中、`Update` は `Verify` より前に
完全なブロックを出力できるため、その時点のバイト列は未認証である。したがってハーネスでは、
`Update` と `Verify` の両方の出力先を常に provider-private な scratch 領域としている。
認証失敗時には scratch 全体をゼロ化し、出力先については試行対象の prefix のみをゼロ化する。
また、文書化された中断／復旧境界として `Close`/`Open` を使用する。ゼロ化ループには、
単純な `memset` ではなく volatile store とコンパイラのメモリバリアを使用している。

FSP 6.5.0 の `R_RSIP_AES_AEAD_LengthsSet()` は CCM 専用であり、GCM で呼び出すと
`FSP_ERR_INVALID_STATE` を返す。GCM の tag 長は 16 バイト固定であるため、ハーネスの
GCM 経路では `LengthsSet()` を呼び出さない。

## raw `K_model` の露出

`K_model` のインポートは実装も実行もしていない。確認した Protected Mode API には、
GCM の plaintext 出力をラップ済み AES 鍵へ直接接続する機能がない。将来、サポート済みの
インポート／プロビジョニング経路を用意した場合でも、エンベロープの復号結果は tag の検証後、
32 バイトの provider-private な CPU RAM バッファとして現れ、直ちにインポートしたうえで
明示的にゼロ化する必要がある。現在の FSP インターフェースにおいて、鍵が完全に RSIP 内部に
とどまると説明するのは誤りである。

## ビルドおよびハードウェア試験手順

通常のビルドでは `MTFS_RA8P1_CRYPTO_SPIKE_ENABLE` を未定義のままにする。
診断用イメージをビルドするには、選択した e2 studio 構成の C および assembler の
preprocessor definitions の両方へ、一時的に `MTFS_RA8P1_CRYPTO_SPIKE_ENABLE=1`
を追加し、clean build 後にボードへ書き込む。セッション終了後はこの定義を削除する。
起動時のコンソールには `TEST KEY - NOT FOR PRODUCTION` と表示されなければならない
（試験鍵がリンクされていないことも同時に表示される）。

現在実行可能なコマンドは、次の三つだけである。

```text
crypto-info
crypto-kat
crypto-negative
```

RFP/SKMTでRA8P1 Protected Modeのinitial AES-256 `.rkey`注入を確認するまで、
実機プロビジョニングを試みてはならない。保存先はSDの`0:/MTFSKEY.BIN`とし、
専用app以外から作成・更新しない。注入経路と実addressを確定した後にのみ、Phase 4.1A の
`golden_package.mtfs` を `GOLDEN.MTF` のような ASCII ファイル名でコピーし、
残りの再起動、実際の完全電源断、mutation、4/16/64 KiB、ゼロ化、性能、および SD の
各試験を実行すること。

## Phase 4.2 への引継ぎ判断

再利用可能なもの: Protected Mode の FSP 選択、必要最小限のアルゴリズム構成、
検証前の出力を private scratch に限定する規則、強化したゼロ化プリミティブ、
Close/Open による中断処理、診断カウンター、および明示的な試験専用ゲート。

新規に設計／検証が必要なもの: 量産用の鍵プロビジョニング、contest profileのSD鍵レコードの
実機試験と電源断時のプロトコル、`K_model` のインポート、古いハンドルのライフサイクル、パッケージパーサー、
SD パイプライン、TrustZone/FullSecure 境界、ハードウェア試験結果、および性能。

これらの項目と、必要な実機試験のすべてが PASS するまで、Phase 4.1B-RA は
**未完了**である。本フェーズの結果を、provider が production-ready であると判断するための
根拠として使用してはならない。

## この worktree で実施した検証

FSP プロジェクトのコンテンツ生成では、Protected Mode 構成が正常に受け付けられた。
Arm GNU 13.2.1 による clean build は、次の構成すべてでコンパイラの warning/error なく完了した。

| 構成 | 機能 | text | data | BSS |
|---|---:|---:|---:|---:|
| Debug | 無効（チェックイン済みの既定値） | 130,240 | 0 | 87,300 |
| Release | 無効（チェックイン済みの既定値） | 91,868 | 0 | 87,312 |
| Debug | 一時的に有効 | 208,096 | 0 | 284,820 |
| Release | 一時的に有効 | 152,676 | 0 | 284,832 |

機能を有効にした Release の map では、private scratch と ciphertext の各バッファが
65,536 バイトの BSS シンボルとして、destination/sentinel バッファが 65,552 バイトとして
配置される。AES-256 のラップ済み値は 52 バイトである。GCC の `-fstack-usage` による報告値は、
コマンドのエントリーポイントが 152 バイト、インライン化されない各 GCM helper が 64 バイトである。
これらはコンパイラによる静的な値であり、ターゲット上で測定した high-water mark ではない。

既定の無効状態の object を復元した後、Debug と Release のどちらの ELF にも、
32 バイトの `fleet_test.key` と完全一致するバイト列、`MTFSMOD`、暗号コマンド名、
試験専用バナーは含まれていなかった。無視対象になっている e2 studio の生成済みビルドメタデータ／
デバッグ情報には絶対パスが残るが、生成ソースおよび手書きソースには、マシン固有の絶対パスを
含むものをコミットしていない。

WSL2 Ubuntu 26.04 での Host 回帰試験結果:

- sealed-model Debug: 1/1 passed
- sealed-model Release: 1/1 passed
- sealed-model ASan/UBSan Debug: 1/1 passed
- 既存の Host suite: 2/2 passed
- 独立した Python `cryptography` によるベクター再生成: 5,272 バイトのパッケージ、
  2 chunks。JSON、package、payload、test key の SHA-256 はすべてバイト単位で一致

機能を有効にした Debug イメージによる初回の実機試行では、起動バナーと `crypto-info` の
表示を確認した。初回は前述のハーネス不具合によりFAILしたが、修正版では
`crypto-kat`と`crypto-negative`の両方で、生成鍵によるempty/partial/4/16/64 KiB、
multi-shot、negative、Close/Open後の再利用がPASSした。両commandは現在同じcombined smokeを
呼ぶため、2回実行後のcounterは`open=4 close=4 wrap=2 start=26 update=94 verify=14
auth_fail=2 abort=2 zeroize=40 generation=4 last_fsp=0`だった。この結果はgenerated-key
hardware smokeのPASSであり、fixed-key KATのPASSではない。また、initial `.rkey`注入、
SDからのfixed-key load、再起動、実際の完全電源断、golden vector、
SD/FatFs 暗号処理、ターゲット性能、および既存の RA smoke/normal runtime も
引き続き **NOT RUN** である。
