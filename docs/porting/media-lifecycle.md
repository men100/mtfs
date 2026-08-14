# Removable-media lifecycle

`src/core/mtfs_media.*` は vendor HAL、GPIO、FatFs、OS に依存しない挿抜状態機械です。
静的 `mtfs_media_context_t` に `ABSENT`、`DEBOUNCING_INSERT`、`PRESENT`、
`DEBOUNCING_REMOVE`、`ERROR` を保持し、確定時だけ `INSERTED`、`REMOVED`、`ERROR` を
通知します。起動時のGPIO読出しは初期状態を決めますが、物理edgeではないため
`INSERTED`/`REMOVED`を生成しません。heapは使用しません。

## IRQとdebounce

targetのCard Detect ISRはraw levelを読み、`mtfs_media_notify_isr()`またはoptional serviceの
`mtfs_media_service_notify_isr()`へ渡します。これらはraw levelとsequenceを記録し、service
版はさらに`tk_set_flg()`するだけです。ISRからevent callback、FatFs、mount/unmount、mutex、
HAL abort/deinit、待機を呼びません。ISR-safeという契約は、microT-Kernelの
`TA_HLNG` task独立部から`tk_set_flg()`可能なtargetに限ります。

workerはedge後にだけ起床し、`debounce_ms`後に`read_signal`でGPIOを再読出しします。
debounce中にsequenceまたはlevelが変われば期限を再設定します。event flagのbitが
coalesceしてもsequenceと最終GPIO再読出しで収束します。常時pollはしません。
IRQがないtargetは、アプリが必要な頻度を明示して`mtfs_media_poll()`を呼べます。
`mtfs_media_notify()`はtask文脈からのmanual notificationです。

## 最小構成とoptional service

最小構成では、アプリ既存のstorage管理taskが自身のevent flagを待ち、
`mtfs_media_process(now_ms, &next_wait_ms)`を呼びます。`next_wait_ms`だけ一度待って再実行し、
`MTFS_MEDIA_WAIT_FOREVER`なら次のedgeまで待機します。この構成は専用taskを作りません。

`src/os/microtkernel/mtfs_media_service.*`を明示的にリンクしcontextを確保すると、同じ処理を
静的user stack付きの1 taskと1 event flagで実行します。既定stackは2048 byteで
`MTFS_MEDIA_SERVICE_STACK_SIZE`により変更できます。sourceをリンクせずcontextを確保しない
構成には、code、stack、task、event flagのコストはありません。serviceもfilesystemを
自動mountしません。

## アプリケーションpolicy

event callbackはworker task文脈です。代表的なpolicyは次です。

- `INSERTED`: Block Device確認、`mtfs_block_initialize()`、registry登録、`f_mount()`。
- `REMOVED`: 新規要求停止、`f_mount(NULL, ...)`、registry解除、portを未初期化状態へ移行。

物理抜去時点で未保存データを救済する保証はありません。open中だった`FIL`は無効として
扱い、再挿入後も透過的に再開しません。新しい媒体または同じ媒体の再挿入後は、明示的な
initialize/register/mountが必要です。自動formatは行いません。

## cleanup順序

1. `mtfs_media_service_stop_notifications()`で新規通知を拒否する。
2. targetのCard Detect IRQをdisableし、`tk_def_int(..., NULL)`等で解除する。
3. `mtfs_media_service_deinit()`でworker停止を確認する。
4. taskとevent flagを削除する。
5. `mtfs_media_deinit()`で共通contextを無効化する。

この順序によりISRが削除済みevent flagを参照するraceを防ぎます。Block Device/HALのdeinitは
Card Detect経路を止めた後、並行I/Oがない通常文脈で行います。
