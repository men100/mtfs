# RA FSP RSIP wrapped-key file

`mtfs_ra_rsip_key_file`は、FatFsでマウント済みのSDから
`MTFSKEY.BIN`を読み、RSIP-E50D Protected Modeの
`rsip_wrapped_key_t`として公開する薄いadapterです。

- default path: `0:/MTFSKEY.BIN`
- provider: RA RSIP-E50D
- key type: AES-256
- wrapped blob: 52 bytes
- record/blob buffer: 16-byte aligned

loaderはファイル形式とCRCを検査しますが、HUKによる真正性の最終判定はRSIP operationが
行います。失敗時とunload時はrecord bufferをzeroizeし、鍵handleを返しません。
SDの初期化、Block Device登録、FatFs mount、RSIP open/closeはapplicationの責務です。
