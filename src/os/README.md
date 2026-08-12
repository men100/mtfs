# OS adaptation

排他、時刻、メモリ、タスク連携など、OS依存機能の適合層を配置します。FatFs本体とOS APIを直接結合せず、`mtfs_config.h` の選択に応じた適合ソースだけが実体を生成します。

Phase 1のmutex適合層は次のいずれかを `MTFS_FATFS_MUTEX_ADAPTER` に指定します。

- `MTFS_FATFS_MUTEX_ADAPTER_NONE`: 既定値。`MTFS_FF_FS_REENTRANT=0` のみで使用可能
- `MTFS_FATFS_MUTEX_ADAPTER_POSIX`: Linux/WSL2ホストテスト用pthread実装
- `MTFS_FATFS_MUTEX_ADAPTER_MICROTKERNEL`: microT-Kernel 3.0用実装

選択値は単一のenum形式なのでPOSIX版とmicroT-Kernel版を同時に有効化できません。`src/` 全体をビルド対象へ加えても、非選択適合層のソースはOSヘッダや関数実体を生成しません。

RTC適合はPhase 1の対象外です。既定の `MTFS_FF_FS_NORTC=1` ではFatFsの固定日時を使用します。
