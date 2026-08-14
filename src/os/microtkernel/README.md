# microT-Kernel 3.0 adaptation

`mtfs_fatfs_mutex.c` はFatFs R0.16のmutex契約をmicroT-Kernel 3.0の公開APIへ接続します。カーネルやBSP本体は複製・変更しません。

有効化例:

```c
#define MTFS_FF_FS_REENTRANT 1
#define MTFS_FF_FS_TIMEOUT 1000
#define MTFS_FATFS_MUTEX_ADAPTER MTFS_FATFS_MUTEX_ADAPTER_MICROTKERNEL
```

対応は `ff_mutex_create` → `tk_cre_mtx`、`ff_mutex_delete` → `tk_del_mtx`、`ff_mutex_take` → `tk_loc_mtx`、`ff_mutex_give` → `tk_unl_mtx` です。`FF_VOLUMES + 1` 個のIDを静的配列で保持し、末尾はFatFsのsystem mutex用です。動的メモリは使いません。生成属性は優先度継承の `TA_INHERIT` です。

この適合層はタスクコンテキストからFatFs APIを呼ぶことを前提にしています。dispatch禁止中、割込みハンドラ、その他 `tk_loc_mtx` が待ち状態へ移行できないコンテキストから使用しないでください。

`mtfs_media_service.c/.h`は明示的に組み込む場合だけ生成されるremovable-media workerです。
静的user stack、1 task、1 event flagを使用し、ISR-safeな通知からdebounce/event callbackを
task文脈へ遅延します。使用しない構成にはobject/RAM costはありません。

`FF_FS_TIMEOUT` はそのままmicroT-Kernelの `TMO` へ渡します。microT-Kernel 3.0の標準timeout値はミリ秒で、実際の起床精度はBSPのsystem timer周期に量子化されます。参照中のBSP2既定設定は `CNF_TIMER_PERIOD=10` msです。設定値は `TMO` の正の範囲に収めてください。

Phase 1のcompile-only確認は、submodule v1.00.04の公開ヘッダ `<tk/tkernel.h>` をinclude pathへ指定し、このadapterを `MTFS_FF_FS_REENTRANT=1` / `MTFS_FATFS_MUTEX_ADAPTER_MICROTKERNEL` でコンパイルする方法で行いました。Phase 2のEK-RA8P1 Debug buildでも同じadapter sourceが実際のtargetへ組み込まれます。RTCは使用せず、`MTFS_FF_FS_NORTC=1` とFatFs固定日時を使用します。
