# Security extensions

鍵管理連携、ポリシー、完全性検証、監査などのセキュリティ拡張を配置します。暗号化 I/O フィルタそのものは `src/block/filters/` と連携します。

- `wrapped_key/`: provider固有のラップ済み鍵を外部メディアへ保存するための、target非依存レコードcodec。実鍵やprovider APIは含みません。
