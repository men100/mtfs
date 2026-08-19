# Renesas RA FSP port

EK-RA8P1 を主対象とする Renesas RA FSP 向けポートです。FSP 共通処理と転送方式、TrustZone 境界を分離します。

- `crypto/`: SD上のprovider-native wrapped keyをRSIP-E50D handleへ接続するadapter
