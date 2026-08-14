# Core

FatFs の利用を調停する microT-FS の中核処理を配置します。マウント、ボリューム管理、同期、エラー変換などの実装領域です。

`mtfs_media.c/.h`はGPIO/HAL/OS非依存のremovable-media状態機械です。edge通知後だけ
debounceし、task文脈でINSERTED/REMOVED/ERRORを通知します。filesystemのmount policyは
持ちません。詳細は`docs/porting/media-lifecycle.md`を参照してください。
