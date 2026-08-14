# RTC timestamp providerの移植

microT-FSはRTC値をlocal calendarとして扱います。timezone、UTC、DSTはapplicationで
処理し、providerには変換済みの年/月/日/時/分/秒を渡してください。

## 共通契約

新しいportは`mtfs_time_provider_ops_t`の`get_local`、`set_local`、`get_status`、`clear`
を実装します。providerとcontextは利用期間中に生存する静的領域へ置き、OS mutexの
lock/unlock callbackを設定してから、task開始時に1回だけregisterします。ISRからRTC APIを
呼ばず、動的メモリも使いません。

状態の意味は次のとおりです。

- `VALID`: marker、RTC read、calendar、FAT年範囲がすべて有効。
- `UNSET`: markerなし、明示clear、backup消失など、設定済みと証明できない。
- `ERROR`: markerはあるがRTC read失敗またはcalendar不正。
- `UNAVAILABLE`: provider未登録、RTC非搭載、またはport未初期化。

RTCの初期値だけで`VALID`にしてはいけません。battery-backed領域にmagic、version/check、
反転値など複数wordのmarkerを置き、commit wordを最後に書きます。設定順序は入力検証、
marker clear、RTC set、RTC readback、同値または最大1秒進みの確認、marker commitです。
途中失敗、backup domain再構成、明示clearではcommit wordを消します。

## FatFs接続

RTCなしtargetは`MTFS_FF_FS_NORTC=1`のままにし、RTC port、`mtfs_time.c`、
`mtfs_fattime.c`をリンクしません。固定日時のまま追加RAM/実行時依存はありません。

RTC targetは次をリンクして`MTFS_FF_FS_NORTC=0`にします。

```text
src/core/mtfs_time.c
src/fatfs/mtfs_fattime.c
src/ports/<vendor>/rtc/<port>.c
```

`get_fattime()`は`VALID`だけをFATの1980-2107年、2秒分解能へpackingし、それ以外では
0を返します。hardware RTCの年範囲が狭い場合、portの`set_local`でさらに制限します。

診断consoleはfilesystem設定とは別のtarget固有switchで選択してください。STM32N6570-DK
test targetでは`MTFS_TARGET_RTC_CONSOLE`を使用し、`MTFS_FF_FS_NORTC=1`ならconsole設定に
かかわらずRTC初期化とconsoleを無効にします。filesystem timestampを使う製品でも、設定用
consoleをリンクまたは起動する必要はありません。

## 保持・security確認

software reset、VDD-off、VBATなし、tamper、backup resetを分けて試験します。reset flagは
startupがclearすることがあるため、markerの代用にはしません。TrustZone targetではRTC setと
marker writeをSecure側に置き、Non-Secure側へは検証済みread serviceを公開する境界を推奨
します。backup registerのsecure/non-secure zone、privilege、vendor isolation controllerも
targetごとに確認してください。
