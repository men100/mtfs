# Sealed model v1 test vectors

**TEST ONLY - NOT FOR PRODUCTION**

このdirectoryは、sealed model writerとreaderが同じ誤りを持った場合にもtestが誤って成功しないよう、
C++実装とは独立したPython `cryptography`のAES-GCM実装で生成した既知の正解値を格納する。
ここでいう`golden`は「固定された正解データ」という意味であり、production用の鍵やmodelではない。

## Package全体とfileの対応

`golden_package.mtfs`は次の5272 byteで構成される。

```text
offset 0
  +------------------------------+ 160 byte
  | preamble                     |
  +------------------------------+  32 byte
  | metadata TLV                 |
  +------------------------------+ offset 192
  | K_model envelope ciphertext  |  32 byte
  +------------------------------+
  | envelope authentication tag  |  16 byte
  +------------------------------+ offset 240
  | chunk 0 ciphertext           | 4096 byte
  +------------------------------+
  | chunk 0 authentication tag   |  16 byte
  +------------------------------+ offset 4352
  | chunk 1 ciphertext           | 904 byte
  +------------------------------+
  | chunk 1 authentication tag   |  16 byte
  +------------------------------+ offset 5272
```

`manifest`はpackage全体ではなく、先頭の`preamble || metadata`、すなわちこのvectorでは
offset 0..191の192 byteである。envelopeと各payload chunkは、この同じmanifestをAADの一部として
認証する。

| file | 内容 | 主な用途 |
|---|---|---|
| `fleet_test.key` | 固定した32-byte test用`K_fleet` | envelopeの暗号化・復号 |
| `golden_payload.bin` | 固定した5000-byteの疑似model | 復号結果のbyte比較 |
| `golden_manifest.hex` | 192-byte manifestのhex表現 | preamble/TLV/AADの検査とC array化 |
| `golden_package.mtfs` | 5272-byteの完成package | readerのknown-answer test |
| `golden_package.hex` | 完成packageと同一byte列のhex表現 | target testへの移植とbyte差分確認 |
| `golden_vector.json` | 鍵、nonce、AAD、ciphertext、tag、mutation位置をfield別に記録 | crypto provider単位の検証 |
| `generate_vectors.py` | 上記fileを再生成する独立generator | vectorの由来と再現性の確認 |

## 固定値の意味

疑似modelは機密情報を含まず、5000 byteを`payload[i] = (i * 7 + 3) & 0xff`で生成する。
chunk sizeは4096 byteなので、payloadは4096-byteのchunk 0と904-byteの短い最終chunk 1になる。

manifest内のmetadataは次のcanonical TLVである。

| type | flags | value | 意味 |
|---:|---:|---|---|
| 1 | `CRITICAL` | little-endian `0x00010000` | runtime ABI version 1.0 |
| 4 | 0 | UTF-8 `golden` | test用display name |

`golden_vector.json`の`model_key_hex`はenvelopeから得られる期待`K_model`であり、packageへ平文では
格納されない。`chunks`はchunkごとのnonce、AAD、ciphertext、tag、期待plaintext長を分離しているため、
RA/ST providerはpackage parserを実装する前でもAES-GCM処理だけを検証できる。
`expected_failures`は完成packageの指定offsetを`xor: 1`で1 bit反転し、認証失敗になることを確認する値である。

## 再生成

このdirectoryで次を実行する。

```sh
python3 generate_vectors.py
```

generator実行時だけPython `cryptography`が必要であり、通常のC++ testはcheck-in済みのbyte列を使う。
`fleet_test.key`をdeviceへinstallしたり、実modelの保護に使用したりしてはならない。
