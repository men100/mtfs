# EK-RA8P1 dedicated key provisioner

信頼できるローカル環境で、32-byteのraw AES-256 `K_fleet`をUART/XMODEM-CRCで一度だけ受信し、
RA8P1のRSIP-E50D Compatibility Modeで直ちにHUK-wrapped keyへ変換する専用アプリです。
変換後の52-byte blobは、EK-RA8P1上の64 MiB Octo-SPI flashへ保存します。

これはコンテスト用の運用です。正規firmware、接続PC、作業場所を信頼し、remote provisioning、
operator認証、debug lock、anti-rollback、lifecycle管理は対象外とします。raw keyを通常版firmware、
SDカード、repository、ログへ保存しません。

## 保存領域

OSPIは`0x90000000`にmemory-mapされます。末尾8 KiBを鍵専用として予約します。

- slot A: offset `0x03FFE000`、address `0x93FFE000`
- slot B: offset `0x03FFF000`、address `0x93FFF000`
- sector size: 4096 bytes
- record: magic、format version、generation、key ID/version、52-byte wrapped key、CRC32
- program: 84-byte logical recordを88 bytesへpaddingし、64 + 24 bytesでpage write

初回はslot Aへ書きます。更新時はinactive slotをerase/write/readback検証してから新generationを
採用するため、途中で電源断しても以前のvalid slotを残します。通常アプリを含め、他の用途が
この末尾8 KiBを消去または書換えてはいけません。失った場合は同じボードを再provisioningできます。
blobを別のRA8P1へコピーしても、そのMCUのHUKでは利用できません。

## Importとbuild

1. e² studioで **File > Import > General > Existing Projects into Workspace** を選ぶ。
2. このdirectoryを指定し、project名`mtfs_ek_ra8p1_key_provision`を確認する。
3. `configuration.xml`を開き、FSP 6.5.0で **Generate Project Content** を実行する。
4. Debugをclean buildする。
5. `Debug/mtfs_ek_ra8p1_key_provision.srec`または`.elf`をボードへ書く。

構成はRSIP-E50D Compatibility Mode、Arm PSA Crypto、key injection、OSPI_Bを使用します。
RFP、SKMT、UFPK/W-UFPK、`.rkey`、MRAM予約は不要です。PSA Cryptoがwrapped-key importと
GCM検証で動的memoryを使うため、BSP heapは`0x3000`（12 KiB）です。`0x200`では
`PSA_ERROR_INSUFFICIENT_MEMORY`（`-141`）になりました。2026-08-20時点のDebug buildは
Arm GCC 13.2.1で成功し、sizeはtext 221,720 bytes、data 88 bytes、BSS 33,549 bytesでした。
FSP 6.5.0の`ra/fsp/src/rm_psa_crypto`だけ既知のunused-variable/function warningを局所抑制し、
applicationを含むそれ以外のsourceでは`-Wall`を維持しています。clean buildはwarning/errorなしです。

## raw key file

送信ファイルはヘッダーや改行を含まない正確に32 bytesのbinaryです。HostのCSPRNGで生成し、
画面やshell historyへhex表示しないでください。アクセス制限した一時ファイルとして扱い、
必要な全ボードのprovisioningが終わったらHost側の正式なkey vault方針に従って保管または破棄します。

## Console

```text
info
verify-ospi
provision-xmodem
update-xmodem
help
```

初回手順は次のとおりです。

1. `verify-ospi`で`not-found`を確認する。
2. `provision-xmodem`を実行する。
3. `XMODEM-CRC ready`表示後、60秒以内に端末ソフトから32-byte binaryをXMODEM-CRCで送信する。
   受信側は送信開始を示す`SOH`または`STX`が届くまで1秒ごとに`C`（CRC request）を再送する。
   128-byte blockと、CoolTermが使用することがあるXMODEM-1Kの1024-byte blockの両方を受け付ける。
4. アプリはraw keyをRAMで受信し、`R_RSIP_AES256_InitialKeyWrap()`でHUK-wrapして直ちにzeroizeする。
5. wrapped keyをvolatile PSA handleへimportし、AES-256-GCMの正例と改変tag拒否を確認する。
6. OSPIへcommitし、readbackしたblobでも同じ暗号試験を行う。
7. `OSPI provision PASS`と`OSPI verify PASS`を確認する。

PSA検証に失敗した場合は、ログの`stage=import`、`encrypt`、`decrypt`、`negative`と
`psa=...`で失敗箇所を確認できます。

FSP 6.5.0の`gcm_alt_process.c`はRSIPの`FSP_ERR_CRYPTO_SCE_AUTHENTICATION`を
`MBEDTLS_ERR_PLATFORM_HW_ACCEL_FAILED`へ一律変換するため、改変tag拒否はPSA標準の
`PSA_ERROR_INVALID_SIGNATURE`（`-149`）ではなく`PSA_ERROR_HARDWARE_FAILURE`（`-147`）に
なります。本実装は、直前の正例decryptが成功した後の意図的なnegative stepに限って
`-147`を認証拒否として受理し、通常の暗号処理で発生した`-147`は失敗のまま扱います。

既存recordがある場合、`provision-xmodem`は上書きしません。鍵更新を意図するときだけ
`update-xmodem`を使用し、key versionとgenerationが増えたことを確認します。
更新後は旧`K_fleet`でsealしたmodel packageを復号できないため、新しいfleet keyでmodelを
再sealしてSDを入れ替えてください。dual slotの旧recordは電源断復旧用であり、通常loaderから
旧keyを選択するrotation機能ではありません。

プロビジョニング後は通常版`tests/targets/ek_ra8p1/basic`を書き込み、完全電源断後に次を実行します。

```text
crypto-info
crypto-consistency
crypto-negative
crypto-package-test
```

`crypto-consistency`はprovisioned keyによるGCM一貫性試験です。`crypto-package-test`と
`crypto-negative`は、同じHost `fleet.key`から生成してSD rootへ置いた`MTFSTEST.MTF`を使い、
packageの既知plaintext照合と改ざん拒否を検証します。公開テスト鍵への更新や試験後の鍵復元は不要です。生成手順は
[`ek-ra8p1-phase-4.1b-ra2.md`](../../../../docs/security/ek-ra8p1-phase-4.1b-ra2.md)を参照してください。
