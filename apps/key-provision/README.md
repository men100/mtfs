# Wrapped-key provisioning application

信頼できる作業場所でのみ使用する、通常版microT-FSとは独立したprovisioning firmwareです。
RFPがRA8P1のMRAMへ注入したHUK-wrapped AES-256 keyをRSIP-E50Dで検証し、
SDの`0:/MTFSKEY.BIN`へ保存します。raw `K_fleet`、UFPK、W-UFPK、`.rkey`は
このdirectoryにもfirmware imageにも含めません。

このアプリケーションは次を行いません。

- SDのformat
- 既存`MTFSKEY.BIN`の上書きまたは削除
- 残存`MTFSKEY.TMP`の自動削除
- MRAMの書込みまたは消去
- lifecycle、debug lock、programming interfaceの変更

target固有の手順は`targets/ek_ra8p1/README.md`を参照してください。
