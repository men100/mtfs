# Test framework

共通のテスト登録、アサーション、実行制御、結果報告を配置します。特定SDKのテスト機能や `stdio.h` へ直接依存させません。

`mtfs_test_begin()` にreporter callbackと任意contextを渡します。callbackには開始、check失敗、終了のeventとtest名、check数、failure数が通知されます。失敗eventではファイル、行番号、メッセージも保持されます。callbackは `NULL` でも安全です。

ホストランナーはcallback内で `printf()` を使います。将来の実機ランナーは同じ契約を `tm_printf()` 等へ接続できます。
