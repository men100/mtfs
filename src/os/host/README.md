# POSIX host adaptation

`mtfs_fatfs_mutex_posix.c` はLinux/WSL2ホストテスト向けのFatFs mutex適合層です。`pthread_mutex_t` を `FF_VOLUMES + 1` 個静的に保持し、最後の要素をFatFsのsystem mutex用に予約します。

`MTFS_FF_FS_REENTRANT=1` と `MTFS_FATFS_MUTEX_ADAPTER_POSIX` を選択して使用します。`FF_FS_TIMEOUT` はミリ秒として `CLOCK_REALTIME` 基準の絶対時刻へ変換し、`pthread_mutex_timedlock()` に渡します。ビルドはCMakeの `Threads::Threads` をリンクしてください。

この実装はLinux/WSL2用です。非選択時はpthreadヘッダも関数実体も生成しないため、実機プロジェクトが `src/` 全体を列挙してもmicroT-Kernel構成と競合しません。
