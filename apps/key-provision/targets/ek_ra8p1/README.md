# EK-RA8P1 dedicated key provisioner

## 状態

単独import可能なe² studio project、source実装、target非依存のSD record/FatFs層は完成している。
FSP 6.5.0／Arm GCC 13.2.1のDebug clean buildは0 errors、0 warningsで完了した。
RFP/SKMTがこのPCへ未導入であり、RA8P1 RSIP-E50D Protected ModeのAES-256 `.rkey`生成・
boot interface注入、実際の注入address、実機起動とprovisioningは未完了である。

`application/mtfs_target_main.c`は通常版test runnerへ組み込まない。専用e² studio projectは
`tests/targets/ek_ra8p1/basic/configuration.xml`と同じSCI_B SPI、PMOD2 pin、RSIP-E50D
Protected Mode構成を基にし、test case、benchmark、RTC consoleはリンクしない。現在のFSP
configurationには検証済みbasic構成を安全に再利用するため、appがopenしないRTCとcard-detect
instanceも残している。

専用projectで直接使用するmicroT-FS側の主要sourceは次のとおり。

- `application/mtfs_target_main.c`
- `src/block/mtfs_block_device.c`、`src/block/mtfs_block_registry.c`、`src/block/mtfs_block_diagnostics.c`
- `src/fatfs/ff.c`、`src/fatfs/mtfs_diskio.c`
- `src/extensions/security/wrapped_key/mtfs_wrapped_key_record.c`
- `src/extensions/security/wrapped_key/mtfs_wrapped_key_fatfs.c`
- `src/ports/ra_fsp/crypto/mtfs_ra_rsip_key_file.c`
- `src/ports/ra_fsp/sd_spi/`のRA SD SPI port
- microT-Kernel用FatFs mutex adapter、EK-RA8P1のvector cache/T-Monitor workaround

既存basic applicationをprovisionerとして配布せず、project名とELF名にも`key_provision`を含める。

## Importと安全なbuild

1. e² studioで **File > Import > General > Existing Projects into Workspace** を選ぶ。
2. この`apps/key-provision/targets/ek_ra8p1` directoryを指定する。
3. project名が`mtfs_ek_ra8p1_key_provision`であることを確認する。
4. `configuration.xml`を開き、FSP 6.5.0を選んで **Generate Project Content** を実行する。
5. Debugをclean buildする。

既定の`MTFS_RA8P1_PROVISION_KEY_ADDRESS=0`を変更せずbuildする。生成物は
`Debug/mtfs_ek_ra8p1_key_provision.elf`と`.srec`である。確認済みDebug sizeは
text 145,424 bytes、data 0 bytes、BSS 14,173 bytesである。

この既定imageは起動確認用である。`info`と`help`は使用できるが、`verify-injected`は
address未設定としてBLOCKEDになる。`provision-sd`も同じ検査でSDをopenする前に停止する。
`verify-sd`だけは既存fileの読取りを試すため、カードへアクセスする。

## 必須build定義

RFP projectで確定した実際の注入addressを、Cとassemblerの両方へ設定する。
addressを推測してはならない。未設定のimageはbuildできるが、注入鍵を読む全commandを
BLOCKEDにする。

```text
MTFS_RA8P1_PROVISION_KEY_ADDRESS=0x........
MTFS_RA8P1_PROVISION_KEY_ID=1
MTFS_RA8P1_PROVISION_KEY_VERSION=1
MTFS_FF_FS_REENTRANT=1
MTFS_FATFS_MUTEX_ADAPTER=2
MTFS_FF_FS_NORTC=1
```

注入領域はこの専用imageと重ならないMRAM範囲に限定する。通常版microT-FSではこの領域を
予約しない。SDへの保存と検証が終わった後、通常版を書き込むことで一時copyが消えてもよい。

## Console

```text
info
verify-injected
verify-sd
provision-sd
help
```

`provision-sd`は次の順序で動く。

1. 注入addressが設定され、16-byte alignedで、52-byte blobがerased patternでないことを確認
2. 注入blobを使ったAES-256-GCM positive/negative test
3. 既存`MTFSKEY.BIN`と`MTFSKEY.TMP`がないことを確認
4. `MTFSKEY.TMP`へ100% writeし、`f_sync()`、close、readback、CRC検証
5. `MTFSKEY.BIN`へrename
6. SDから再loadしたblobを使って同じRSIP test

途中で失敗したtemporary fileは自動削除しない。カードをPCで調査し、必要なら明示的に
削除してから再試行する。`MTFSKEY.BIN`が存在する場合も上書きせず停止する。

## RFP/SKMT preflight gate

実行前に、導入したRFP/SKMTで以下を確認する。

- targetがRA8P1 / RSIP-E50D Protected Modeである
- initial AES-256 user keyの`.rkey`生成とboot interface注入が選択可能である
- RFP verifyが成功し、注入後の52 bytesをapplicationからwrapped keyとして利用できる
- programming interfaceとOEM lifecycleをcontestの再provision方針に合う状態で維持する

このgateが通るまで、アドレスを定義して実機で`verify-injected`または`provision-sd`を
実行してはならない。
