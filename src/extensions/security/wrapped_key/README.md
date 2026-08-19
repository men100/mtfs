# Wrapped-key record

provider固有のラップ済み鍵blobをSDなどの外部メディアへ保存するための、
固定長header codecです。暗号処理、ファイルI/O、provisioningは担当しません。

Version 1の整数はlittle-endianです。RSIP-E50D AES-256ではheader 32 bytesと
wrapped blob 52 bytesを合わせた84 bytesになります。

| offset | bytes | field |
|---:|---:|---|
| 0 | 8 | ASCII `MTFSWKEY` |
| 8 | 2 | format version (`1`) |
| 10 | 2 | header bytes (`32`) |
| 12 | 4 | provider (`RA_RSIP_E50D=1`) |
| 16 | 4 | key type (`AES_256=1`) |
| 20 | 4 | public key ID |
| 24 | 4 | public key version |
| 28 | 4 | IEEE CRC-32 |
| 32 | provider-specific | HUK-wrapped key blob |

CRC-32はheaderのoffset 0..27とblobを対象にします。これはSD破損の診断用であり、
セキュリティ境界ではありません。blobの暗号学的な真正性はproviderが検証します。
decoderはmagic、version、header長、provider、key type、正確なファイル長、CRCを
すべて検査し、末尾dataを許可しません。

`mtfs_wrapped_key_fatfs_create()`は既存のdestinationとtemporary fileを上書きせず、
temporary fileへwrite、`f_sync()`、close、readback検証を行ってからdestinationへ
renameします。失敗したtemporary fileは診断と明示的な復旧のため残し、libraryが
自動削除することはありません。filesystemのformatも行いません。
