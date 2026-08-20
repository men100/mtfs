# RA8P1 wrapped-key storage

`mtfs_ra8p1_ospi_key_store`は、EK-RA8P1 onboard 64 MiB Octo-SPI NORの末尾8 KiBを
HUK-wrapped AES-256 fleet key専用領域として扱うtarget adapterです。

- OSPI memory-map base: `0x90000000`
- slot A/B offsets: `0x03FFE000` / `0x03FFF000`
- erase unit: 4096 bytes
- wrapped value: 52 bytes (`rsip_aes_wrapped_key_t`)
- record: versioned metadata + CRC32、explicit little-endian

初回commitは既存recordを上書きしません。更新はinactive slotへ書いてreadback検証するため、
旧slotを先に壊しません。loaderはvalidな最大generationを選びます。CRCは偶発的破損の検出であり、
攻撃者に対するMACやanti-rollbackではありません。wrapped keyのdevice bindingと最終的な有効性は
RSIP-E50D/PSA operationで確認します。

この領域を他のOSPI用途、filesystem、全chip eraseと共有してはいけません。鍵素材を保持する
一時bufferは`mtfs_ra8p1_ospi_key_store_zero()`で明示的にzeroizeします。
