# EK-RA8P1 RSIP-E50D hardware crypto spike（Phase 4.1B-RA）

状態: **CONTEST PROFILE CORE CRYPTO PATH HARDWARE PASS / EXTENDED ACCEPTANCE PENDING**
（2026-08-20）。

## 経緯と現在の判断

最初のspikeはRSIP-E50D Protected Modeでhardware-generated keyによるAES-256-GCMを実行し、
empty、37 byte、4/16/64 KiB、改変tag、Close/Openを実機でPASSした。一方、固定`K_fleet`を
Protected Modeへ入れるにはKUK/UFPK/W-UFPK、SKMT/RFP、DLM Key Wrap Service等を含む
production寄りの手順が必要になり、コンテスト利用者の導入負担に対して利点が小さかった。

今回のthreat modelは正規firmwareと信頼できるローカルprovisioning環境を前提とし、目的は
「removable SDの漏洩からAI modelを守る」ことである。この目的には、初回だけraw `K_fleet`を
ボードへ転送し、その場でMCU固有HUKにwrapするCompatibility Modeで足りる。そのため実装を
次へ変更した。

- provider: RSIP-E50D Compatibility Mode + Arm PSA Crypto
- initial input: dedicated appのUART/XMODEM-CRC、正確に32-byte
- wrapping: `R_RSIP_AES256_InitialKeyWrap(RSIP_KEY_INJECTION_TYPE_PLAIN, ...)`
- persistent key: board上OSPI末尾8 KiBのdual-slot record
- runtime import: 52-byte blobをvolatile `PSA_KEY_TYPE_AES_WRAPPED` handleへimport
- model storage: SD（鍵blobはSDへ置かない）

具体的なprovisioning境界、OSPI layout、受入れ条件は
[`ek-ra8p1-ospi-key-provisioning.md`](ek-ra8p1-ospi-key-provisioning.md)を参照する。

## FSP構成

`apps/key-provision/targets/ek_ra8p1`と`tests/targets/ek_ra8p1/basic`はFSP 6.5.0で次を選ぶ。

- Arm MbedCrypto PSA implementation
- `rm_psa_crypto`
- `r_rsip_e50d_ra8_plaintext`
- `r_rsip_e50d_key_injection`
- `r_ospi_b` unit 0 / channel 1

OSPIはEK-RA8P1 onboard 512-Mbit NORをCS1、base `0x90000000`、4-byte address、standard SPIで
使用する。fast read `0x0C`（8 dummy cycles）、program `0x12`、sector erase `0x21`、
sector size 4096 bytesである。
Data cacheはBSP設定で有効である。OSPI recordのmemory-mapped read前には対象cache lineだけを
invalidateし、erase/program後の古い内容を検証しないようにする。

FSP packで確認したAES-256 wrapped valueは
`R_RSIP_AES256_KEY_INDEX_WORD_SIZE * 4 = 52` bytesである。applicationはHUKを読まず、
HUK選択とwrap/unwrapはRSIP内部で行われる。ただしCompatibility provisioning中のraw keyは
CPU RAMに一時的に存在するため、この構成をhardware隔離済みproduction provisioningとは呼ばない。

## Console test

通常版では`MTFS_RA8P1_CRYPTO_SPIKE_ENABLE=1`がDebug/ReleaseのC定義に設定され、次を公開する。

```text
crypto-info
crypto-consistency
crypto-negative
```

`crypto-info`はCompatibility Mode、OSPI status、valid slot数、key ID/version、generation、slot、
PSA/OSPI診断を表示する。鍵素材は表示しない。recordがなければ専用provisionerを実行するよう
明示してBLOCKEDにする。

`crypto-consistency`は保存済み`K_fleet`を使うGCM正例試験である。empty、37 byte、4/16/64 KiBを
encrypt/decryptし、さらにhandleをdestroyしてOSPIを再読込み・再importした後も既存ciphertextを
復号できることを確認する。未知のprovisioned keyを使うround-trip/consistency testであり、
公開固定vectorとの厳密なknown-answer test（KAT）ではない。

`crypto-negative`は37-byte plaintextを暗号化し、tagの1 bit改変とciphertextの1 bit改変を
それぞれ`PSA_ERROR_INVALID_SIGNATURE`として拒否することを確認する。失敗時はdestination全体を
明示的にzeroizeし、出力長が0であることを確認する。

## Build確認

2026-08-20にFSP project contentを再生成し、Arm GCC 13.2.1 Debugでリンクした。

| image | text | data | BSS |
|---|---:|---:|---:|
| dedicated provisioner | 221,720 | 88 | 33,549 |
| basic runtime + crypto tests | 315,300 | 88 | 304,497 |

両buildともapplication errorはない。両projectともFSP 6.5.0の
`ra/fsp/src/rm_psa_crypto`だけ既知のunused-variable/function warningを局所抑制し、
application側の`-Wall`を維持したclean buildでwarning/errorなしを確認した。
basicの大きなBSSは64 KiB級のplaintext/ciphertext/recovered test bufferによる。
両projectはPSA Crypto用のBSP heapを`0x3000`（12 KiB）確保する。従来のprovisionerの
`0x200`（512 bytes）では、wrapped-key検証が`PSA_ERROR_INSUFFICIENT_MEMORY`（`-141`）で失敗した。
basicはTrustZoneを使わない単一の信頼済みfirmwareで、PSAへ渡すbufferがprivateかつ非overlapである
前提をcompile definition `MBEDTLS_PSA_ASSUME_EXCLUSIVE_BUFFERS`で明示する。これによりPSAの
whole-message境界copyを省き、12 KiB heapのまま16/64 KiBを処理できる。TrustZone、shared memory、
または信頼できないPSA callerを導入する場合、この定義を外してsecure側buffer設計を見直すこと。

実機ではUART/XMODEM-1K受信、HUK wrap、OSPI erase/write/readback、reset後の再読込み、
provisioned keyのempty/37 byte/4/16/64 KiB GCM consistencyと再import、tag/ciphertext改ざん拒否、
出力zeroizeをPASSした。contest threat modelのcore crypto pathはhardware-validatedとする。
完全電源断、cross-device rejection、key更新中の電源断耐性はextended acceptanceとして残る。
