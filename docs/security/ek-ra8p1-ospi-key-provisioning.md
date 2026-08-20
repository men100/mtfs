# EK-RA8P1 OSPI fleet-key provisioning profile

状態: **CONTEST PROFILE HARDWARE PASS / OPTIONAL EXTENDED TESTS DEFERRED**
（2026-08-20）。

## 決定

EK-RA8P1コンテスト構成はRSIP-E50D Compatibility Modeを使う。信頼できるオフィスまたは
自宅で専用provisioning firmwareを起動し、Hostで管理する32-byte AES-256 `K_fleet`を
UART/XMODEM-CRCでボードへ転送する。receiverは128-byte blockとXMODEM-1Kの1024-byte blockを
受け付ける。専用appはraw keyをRAM上で
`R_RSIP_AES256_InitialKeyWrap(RSIP_KEY_INJECTION_TYPE_PLAIN, ...)`へ渡し、MCU固有HUKで
保護された52-byte wrapped keyへ変換後、raw bufferを直ちにzeroizeする。

wrapped keyはEK-RA8P1上の64 MiB Octo-SPI NOR flash末尾8 KiBに保存する。AI model package、
暗号化された`K_model` envelope、payloadは従来どおりremovable SDに置く。SDだけを取得しても
OSPIのdevice-bound `K_fleet`がなければmodelを復号できない。

Protected Mode、KUK、UFPK/W-UFPK、DLM Key Wrap Service、SKMT、RFP `.rkey`注入、内部MRAMの
予約は、このcontest profileでは使用しない。これらは製品向けprovisioningを設計する際の候補であり、
Compatibility Modeによるtrusted local plaintext inputをproduction provisioningと表現してはならない。

## 信頼境界と対象外

信頼するもの:

- provisioningを行う部屋、PC、ケーブル、端末ソフト、作業者
- 正規provisionerと通常firmware
- RSIP-E50D、HUK、OSPI deviceが仕様どおり動くこと

保護するもの:

- SDの紛失、盗難、byte-for-byte複製からのAI model内容
- OSPIから読んだwrapped blobを別MCUへコピーする攻撃

対象外:

- provisioning中のPC/UART/RAM観測
- 悪意あるfirmware、debugger、侵襲解析、side channel
- remote provisioning、operator/device認証、anti-rollback、debug lock、lifecycle変更
- OSPIの物理破損・消去に対する可用性

OSPIのblobは秘密のraw keyそのものではなくHUK-boundだが、改変・破損・削除には耐える必要がある。
可用性を失った場合は同じ`K_fleet`を同じボードへ再provisioningする。

## OSPI layoutと更新

- memory-map base: `0x90000000`
- flash capacity: `0x04000000` bytes
- slot A: offset `0x03FFE000`
- slot B: offset `0x03FFF000`
- erase sector: 4096 bytes

各recordはmagic、format/header size、generation、key ID、key version、wrapped-key length、CRC32、
52-byte wrapped keyを明示的なlittle-endian形式で持つ。論理recordは84 bytesで、OSPI_Bの
64-byte program pageとCPU accessの8-byte単位を守るため、末尾を`0xff`で88 bytesへpaddingし、
64 + 24 bytesの2回に分けて書く。loaderは両slotを検証し、最大generationを選ぶ。

初回provisionはvalid recordがあれば拒否する。更新はinactive sectorだけをeraseし、新recordをwrite、
busy完了待ち、mapped readback、CRCとbyte一致を検証する。新recordが完成するまで旧slotは残る。
これは突然の電源断に対する可用性を改善するが、悪意あるrollbackを防ぐものではない。
また、旧slotを通常運用で選択するkey ringではない。`update-xmodem`で`K_fleet`を変更した後は、
旧keyでsealしたmodel packageを新keyで再sealする必要がある。

この末尾8 KiBはboard resource map上でmicroT-FS専用に予約し、通常アプリ、サンプル、flash filesystem、
全chip erase手順から除外しなければならない。別アプリで消去した場合は再provisioningが必要になる。

## 実装

- `apps/key-provision/targets/ek_ra8p1`: 専用XMODEM provisioner
- `src/ports/ra_fsp/crypto/mtfs_ra8p1_ospi_key_store.*`: dual-slot OSPI record
- `tests/targets/ek_ra8p1/basic/application/mtfs_ra8p1_crypto_spike.c`: 通常版loaderと試験

専用appは受信後、保存前とreadback後の両方でwrapped keyをPSAのvolatile
`PSA_KEY_TYPE_AES_WRAPPED` handleへimportし、AES-256-GCM正例とtag改ざん拒否を検証する。
ログにはraw key、wrapped value、nonce、tag、plaintextを表示しない。
PSA Crypto用のBSP heapは`0x3000`（12 KiB）とする。検証失敗時はPSA statusに加えて
`import`、`encrypt`、`decrypt`、`negative`のstageを表示する。
FSP 6.5.0はRSIPのGCM認証不一致をPSAの`PSA_ERROR_HARDWARE_FAILURE`（`-147`）へ変換する。
この値を認証拒否として受理するのは、同じkey/ciphertextの正例decrypt成功直後にtagまたは
ciphertextを意図的に1 bit改変したnegative testだけとする。通常経路の`-147`は失敗である。
またCompatibility GCMのfinal primitiveは、論理plaintext長を越えて次の16-byte境界まで
outputへ書く。復号bufferとPSAへ渡すcapacityは常にその最終blockを含める。特に64 KiBの
block-aligned入力でも追加16 bytesが必要である。

## Contest core acceptance

1. 未provision状態で`verify-ospi`が`not-found`になる。
2. `provision-xmodem`で正確に32-byteのbinaryを送り、commit/verifyがPASSする。
3. 同じcommandの再実行が`already-provisioned`で書込みなしに停止する。
4. 完全電源断後、通常版の`crypto-info`がkey ID/version/generationを表示する。
5. `crypto-consistency`がempty、37 byte、4/16/64 KiB、OSPI再読込み・再importをPASSする。
6. Phase 4.1B baselineの37-byte GCM negative pathが改変tagとciphertextを拒否し、出力zeroizeを
   PASSする。現行consoleの`crypto-negative`は、`MTFSTEST.MTF`のenvelope tag、chunk ciphertext、
   chunk tagまで検証するPhase 4.1B-RA2の上位統合試験である。

## Extended acceptance

7. wrapped recordを別のRA8P1へコピーした場合、key importまたはGCM operationが失敗する。
8. `update-xmodem`中の各電源断点で、旧slotまたは新slotの少なくとも一方を利用できる。

現時点では両projectのFSP生成とDebug linkに加えて、UART/XMODEM-1K受信、HUK wrap、
GCM正例・negative認証拒否、OSPI slot Aへの初回commit、reset後の`verify-ospi`を実機でPASSした。
通常版loaderによるempty/37 byte/4/16/64 KiB consistencyと再import、tag/ciphertext改ざん拒否、
出力zeroizeに加え、完全電源断後のOSPI key再読込みと同suiteも実機でPASSした。
cross-device rejectionと更新中の電源断耐性は、現在のcontest scopeを妨げないoptional extended testとする。
通常版の初回試験で64 KiB復号bufferのfinal-block余白不足を検出し、16-byte余白とcase/stage別診断を
追加した。さらに16 KiB暗号化時、PSAのwhole-message境界copyが12 KiB heapを超えて`-141`となるため、
flat trusted build限定の`MBEDTLS_PSA_ASSUME_EXCLUSIVE_BUFFERS`を有効化し、修正版でPASSを確認した。
