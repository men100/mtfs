# microT-FS アーキテクチャ

このdirectoryは、microT-FSの責務や各componentの境界、lifecycle、optional featureの全体像を把握するための入口です。

- [全体レイヤー](overview.md): FatFs upstream、microT-FS固有部分、hardwareまでの責務分担
- [通常I/Oとremovable media](io-and-media.md): disk I/Oの経路、登録からmountまでのlifecycle、Card Detectの処理
- [sealed model](sealed-model.md): package構造、hardware-backed key operation、認証済みRAMへのload、security boundary
- [Storage Sentinel](storage-sentinel.md): passive observation、32-window baseline、OOD／rule／CPU／NPUによる判定
- [target profile](target-profiles.md): EK-RA8P1とSTM32N6570-DKで正式に対応するreference profile

portableなcommon APIとtarget固有portは明確に分離されています。applicationは共通の`mtfs_block_device_t`、registry、media、diagnosticsを利用し、board固有の初期化やIRQ接続はtarget portへ委ねます。
