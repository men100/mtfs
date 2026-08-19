# EK-RA8P1 SD wrapped-key provisioning

状態: **DEDICATED TARGET BUILD PASS / HOST-VALIDATED / TARGET PROVISIONING NOT RUN / NOT PRODUCTION-READY**
（2026-08-19）。

## Contest profile decision

EK-RA8P1ごとの初回provisioningを信頼できるオフィスまたは自宅で実施し、通常運用では
HUK-wrapped `K_fleet`をSDの`0:/MTFSKEY.BIN`から読み込む。raw `K_fleet`をtarget firmware、
SD、Git、console logへ保存しない。現地更新用KUK、remote update、anti-rollbackは実装しない。

このprofileは次を信頼する。

- provisioning PC、RFP/SKMT、USB/JTAG接続、作業者
- 正規firmwareとRSIP-E50D
- ボードとSDを一体で使用する通常運用

次は対象外である。

- SD fileの削除、紛失、rollbackに対する可用性保証
- ボードごと盗まれた場合の利用阻止
- programming interfaceを利用できる攻撃者
- production lifecycle、debug lock、量産監査

SDのwrapped blobはHUKによりMCU個体へbindされる。同じ`K_fleet`を注入してもblobは
ボードごとに異なり、別のRA8P1へcopyして利用できない。SD紛失時は再provisioningする。
MRAMを全消去しても同じMCUのHUKは残るため、SDが残っていれば通常版firmwareの再書込みだけで
復旧できることを実機acceptance testで確認する。

## Components

- `src/extensions/security/wrapped_key/mtfs_wrapped_key_record.*`
  - 32-byte header、52-byte RSIP blob、IEEE CRC-32からなる84-byte record
- `src/extensions/security/wrapped_key/mtfs_wrapped_key_fatfs.*`
  - create-new、sync、readback、renameと通常load
- `src/ports/ra_fsp/crypto/mtfs_ra_rsip_key_file.*`
  - aligned bufferと`rsip_wrapped_key_t` adapter
- `apps/key-provision/targets/ek_ra8p1/application/mtfs_target_main.c`
  - RFP注入blobのRSIP検証とSD保存だけを行う専用console application

専用e² studio project `mtfs_ek_ra8p1_key_provision`はFSP 6.5.0／Arm GCC 13.2.1の
Debug clean buildで0 errors、0 warningsを確認した。既定addressは0であり、生成ELFは
実鍵領域を読まず、`provision-sd`をSD open前にBLOCKEDにする。実機起動は未確認である。

CRC-32は媒体破損を診断するためのものであり、security MACではない。wrapped blobの真正性は
RSIPがHUKと内部MACで判定する。

## Provisioning flow

1. RFP/SKMTでRA8P1 / RSIP-E50D Protected Modeのinitial AES-256 `.rkey`対応を確認する。
2. trusted PC上で`K_fleet`と`.rkey`を管理する。repositoryへ追加しない。
3. 専用provisioning imageと`.rkey`を、重ならない一時MRAM addressへRFPで書き込む。
4. 専用consoleの`verify-injected`を実行する。
5. fresh SDを挿入し、`provision-sd`を一度だけ実行する。
6. `verify-sd`と完全な電源断後の`verify-sd`を実行する。
7. 通常版microT-FSを書き込み、SDからfixed fleet keyをloadしてKAT/negative testを行う。

専用appはformat、既存destinationの上書き、temporary fileの自動削除、MRAM write/eraseを行わない。
鍵更新時はtrusted環境で新しい`.rkey`を再注入し、別のfresh SDまたは明示的に整理したSDへ
新しいblobと新しい鍵で暗号化したmodel packageを一緒に配置する。

## Remaining Go/No-Go gate

このPCにはRFP/SKMTが導入されていない。FSP 6.5.0のruntime RSIP-E50D Protected Modeは
AES-256 wrapped keyとAES-GCMをsupportするが、RA8P1 factory boot interfaceでinitial
AES-256 `.rkey`を実際に受け付けることは、RFP/SKMT導入後に別途確認する。

この確認と実際の注入address確定までは、専用appの
`MTFS_RA8P1_PROVISION_KEY_ADDRESS`を0のまま維持する。0の場合、MRAMを読むcommandは
明示的にBLOCKEDとなり、SD writeも行われない。

## Acceptance tests

- host codec: valid record、magic/provider/CRC mutation、truncation、trailing byte
- host FatFs: create/sync/readback/rename、destination上書き拒否、stale temporary保持
- target injected key: GCM positive、modified-tag authentication failure
- target SD: save、readback、power cycle、missing/corrupt/foreign-board blob rejection
- recovery: MRAM全消去と通常版再書込み後、元のSDでfixed-key testがPASS
- rotation: 新key blobと新model packageの同時交換

すべてのtarget項目が実機でPASSするまでは、fixed fleet key経路をhardware-validatedまたは
production-readyと表現しない。
