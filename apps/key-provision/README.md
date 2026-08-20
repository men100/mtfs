# Wrapped-key provisioning application

信頼できる作業場所でのみ使用する、通常版microT-FSとは独立したprovisioning firmwareです。
32-byteのraw AES-256 keyをUART/XMODEMで一時受信し、RA8P1 RSIP-E50D Compatibility Modeで
HUK-wrapped keyへ変換して、ボード上のOSPI flashへ保存します。raw `K_fleet`はこのdirectoryにも
firmware imageにも含めません。

このアプリケーションは次を行いません。

- SDのformatまたは鍵保存
- 初回commandによる既存OSPI recordの上書き
- MRAMの書込みまたは消去
- lifecycle、debug lock、programming interfaceの変更

target固有の手順は`targets/ek_ra8p1/README.md`を参照してください。
