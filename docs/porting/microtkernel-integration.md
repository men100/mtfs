# microT-Kernel統合

microT-FSの実機構成には、FatFs volume mutexとBlock Device内部mutexという2段の
排他があります。両者の役割と割込みからの完了通知を分けます。

## mutexの責務と順序

`MTFS_FF_FS_REENTRANT=1`ではFatFsがvolumeごとの内部状態をvolume mutexで保護します。
`src/os/microtkernel/mtfs_fatfs_mutex.c`は`ff_mutex_create/delete/take/give`を
`tk_cre_mtx`、`tk_del_mtx`、`tk_loc_mtx`、`tk_unl_mtx`へ接続し、
`FF_VOLUMES + 1`個のIDを静的配列に保持します。mutex属性は`TA_INHERIT`です。

port mutexはHAL handle、SPI transaction、SDMMC transfer、bounce bufferなど、1つの
Block Device context内の状態を保護します。RA SPIとSTM32 SDMMCも`TA_INHERIT`を使います。

通常のlock順序は次です。

```text
FatFs API
  -> FatFs volume mutex
    -> mtfs_diskio / Block Device
      -> port access mutex
```

port mutexを保持したままFatFs APIを呼ばないでください。registryの登録/解除はこの
lock階層の対象ではなく、I/O task開始前/停止後に行います。`FF_FS_REENTRANT`は同じ
volumeのFatFs内部状態を保護しますが、同じ`FIL`を複数taskで共有する契約ではありません。

`FF_FS_TIMEOUT`はmicroT-Kernel adapterで`TMO`へそのまま渡されます。既存BSP2の
timeout単位はmsですが、実際の起床精度はsystem timer周期に量子化されます。portの
`io_timeout_ms`/`transfer_timeout_ms`も有限値にし、どの待ちがtimeoutしたか診断できる
状態を残します。

## event flagとIRQ

非同期HALを同期Block Deviceとして見せる場合、task側は転送開始後にevent flagを待ち、
IRQ/HAL callback側は完了bitまたはerror bitだけを設定します。callbackはmutexを取らず、
FatFs API、deinit、長時間処理を呼びません。転送開始前に古いbitをclearし、timeout時は
HALをabortして、遅延callbackが次の転送を完了扱いしない状態管理が必要です。

STM32 SDMMCは`tk_def_int()`でSDMMC IRQを`TA_HLNG` handlerとして登録し、そのhandler
から`HAL_SD_IRQHandler()`を呼びます。HAL callbackはhandle一致とactive transferを
確認して`tk_set_flg()`します。新targetでも、使用するmicroT-Kernel APIがtask独立部から
呼出し可能か、BSPのIRQ entryがmicroT-Kernel管理下かを確認してください。

STM32N6570-DKのCard DetectもPN12/EXTI12を別の`TA_HLNG` handlerとして登録します。
ISRはEXTI pending clear、raw level/sequence記録、media serviceと転送待ちevent flagの
設定だけを行います。debounce、event callback、`HAL_SD_Abort()`はworker/I/O task文脈です。
SDMMC2はpriority 5、EXTI12はpriority 6で、両IRQの診断counterも分離しています。

optional `mtfs_media_service`は静的user stack、1 task、1 event flagを明示init時だけ生成します。
cleanupは通知拒否、source IRQ disable/解除、worker停止、task/event flag削除、media context
無効化の順です。serviceを使わず、既存storage taskが`mtfs_media_process()`を呼ぶ構成も可能です。
workerのSTOPPED通知は、以後context/event flagへ触れない最終境界です。高優先度のcleanup taskが
その通知で先にdispatchされ、workerが`tk_ext_tsk()`へ到達する前でも、serviceはworkerを
`tk_ter_tsk()`でDORMANTにしてから削除します。task削除に失敗したcontext/stackは再利用しません。

RA FSP SPIはFSP生成IRQから`mtfs_ra_sd_spi_callback()`へ入り、callbackがevent flagを
設定します。IRQ番号/priority/callback設定はFSP生成側の責務です。STM32方式の
`tk_def_int()`登録と同じだとみなさず、FSP/BSPが割込みentryとmicroT-Kernel task独立部の
契約を満たすことをtargetごとに確認します。

## HAL timebase

STM32 HALのtimeoutは`HAL_GetTick()`を前提にします。一方、microT-KernelはSysTickを
所有します。`src/ports/stm32_cube/common/mtfs_stm32_hal_timebase.c`は
`CNF_TIMER_PERIOD`が1/10/100 msのとき、`TA_HLNG | TA_STA` cyclic handlerで
`HAL_IncTick()`を呼びます。`manage_hal_timebase=1`ならSDMMC context init/deinitが参照
count付きでacquire/releaseします。`HAL_SetTickFreq()`はSysTickを再構成するため使いません。

すでにapplicationが正しいHAL timebaseを提供する場合は`manage_hal_timebase=0`とし、
二重にtickを進めないでください。`uwTick`がkernel起動後にも増加し、HAL timeoutの単位が
port設定と一致することを実機で確認します。

## task、stack、object lifetime

`usermain()`がreturnするとmicroT-Kernel shutdownへ進みます。既存runnerはcoordinator
taskを開始した後、initial taskを無期限sleepさせています。製品applicationでは意図した
shutdown方針に合わせてinitial taskの寿命を決めます。

FatFsの呼出し深さ、vendor HAL、local bufferを含めてtask stackを見積もります。大きな
sector/DMA/work bufferをautomatic変数へ置かず、context/BSSへ静的配置します。既存の
microT-Kernel並行testは各workerに16 KiBの静的user stackと独立した`FIL`/bufferを持たせます。

worker cleanupは次の順序です。

1. 完了eventを受け、workerが共有資源を更新し終えたことを確認する。
2. 待機状態のworkerを`tk_ter_tsk()`でterminateする。
3. open中の`FIL`が残るerror経路ではcoordinatorが閉じる。
4. `tk_del_tsk()`後に共有event flagを削除する。
5. unmount、registry解除、port deinitを行う。

taskがevent flagを参照中のままflagを削除しないでください。mutex、event flag、cyclic
handler、IRQ登録の生成途中で失敗した場合も、作成済みobjectだけを逆順に解放します。
既存adapter/portはkernel object IDを固定context/配列に持ち、heapを使いません。

## target統合チェックリスト

- [ ] `MTFS_FF_FS_REENTRANT=1`とmicroT-Kernel adapterを同時に設定した
- [ ] FatFs mutexとport mutexのlock順序を守る
- [ ] priority inheritanceを必要なmutexへ設定した
- [ ] IRQ/callbackは待ちやFatFs APIを実行せず、完了通知だけを行う
- [ ] IRQをmicroT-Kernel/BSPが要求する方法で登録した
- [ ] `TA_HLNG`/task独立部から呼べるAPIを確認した
- [ ] timeout単位とHAL timebaseを確認した
- [ ] initial taskが意図せずreturnしない
- [ ] stackと大きなbufferの配置を確認した
- [ ] task終了後に同期objectを削除する
- [ ] init失敗/deinitの全経路でkernel objectを解放する
