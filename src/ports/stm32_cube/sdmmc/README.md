# STM32 SDMMC block device

`mtfs_stm32_sdmmc_context_t` を静的確保し、`mtfs_stm32_sdmmc_context_init()` に `SD_HandleTypeDef *`、IRQ 番号、媒体検出、書込み保護、IDMA 利用可否を渡します。取得した `mtfs_block_device_t` は registry / FatFs disk bridge から通常の同期 Block Device として使用できます。`hsd2` や `SDMMC2_IRQn` はポートに固定していません。

転送経路:

- `use_idma=1`: `HAL_SD_ReadBlocks_DMA()` / `HAL_SD_WriteBlocks_DMA()`、SDMMC IRQ、HAL 完了 callback、T-Kernel event flag で同期化する既定経路。
- `use_idma=0`: 同じ API と mutex を保ち、`HAL_SD_ReadBlocks()` / `HAL_SD_WriteBlocks()` を 1 sector ずつ呼ぶ polling fallback。複数 sector の要求も単一 block command に分割し、速度より確実な復旧性を優先する。
- 1 回の転送は最大 8 sector。単一・複数 block の開始回数、最大 block 数、IRQ/callback/timeout/abort は `diagnostics` で観測できる。

IDMA では context 内の 4096-byte bounce buffer だけを DMA 対象にします。buffer は 32-byte aligned かつ cache-line 完結で、write は copy 後 clean、read は転送前 clean/invalidate、完了後 invalidate してから user buffer へ copy します。FatFs が未整列 buffer を渡しても隣接 cache line を破壊しません。実際の配置領域は linker map と実機ログの両方で確認してください。

同期と復旧:

- FatFs volume mutex → Block Device 呼出し → SDMMC access mutex の順です。SDMMC mutex を保持したまま FatFs API を呼ばないでください。
- HAL callback/IRQ は mutex を取らず event flag のみを更新します。
- Card Detect ISRは`mtfs_stm32_sdmmc_media_changed_isr()`へ抜去hintを渡せます。IDMA待機は
  removal bitで即時解除され、`HAL_SD_Abort()`は起床した通常I/O文脈で実行されます。
- 挿入接点のbounceで一時的なraw抜去hintが残る場合があります。明示initializeはI/O mutex
  取得後に古いremoval pending/eventをclearしてからHAL DeInit/Initを行います。確定挿入だけで
  initializedを復元することはありません。
- IDMA 完了待ちと card-transfer 状態待ちには独立 timeout があり、失敗時は abort して未初期化へ戻します。論理初期化状態とは別に HAL 初期化状態を保持し、deinit または次回 initialize で HAL を確実にリセットしてから復旧します。
- deinit は IRQ、T-Kernel object、HAL、任意の HAL timebase を解放します。並行 I/O がない状態で呼んでください。

制約:

- HAL の global SD callback を handle 一致で dispatch するため、IDMA context は同時に 1 instance です。
- geometry は HAL card info から sector count/size を取得し、erase block は保守的に 1 sector、trim は unsupported です。
- card-detect / write-protect は target callback の能力に従います。`card_present=NULL`は
  従来互換の常時present契約で、IRQ通知APIは使用しません。検出あり構成では抜去後の
  status/read/write/sync/geometryがNO_MEDIAまたは未初期化を返し、geometryを無効扱いにします。
  再挿入だけではinitializedへ戻らず、明示initializeがHAL DeInit/Initして復旧します。
- polling経路はsector間とHAL呼出し後に抜去を確認します。同期HAL呼出しそのものをIRQで
  中断できないため、active polling転送の停止時間はHAL timeoutで上限されます。
- 物理抜去後の未保存data、open `FIL`、書込み中媒体の整合性は保証しません。mount/unmountは
  application policyです。
- `idma_platform_ready` は RIF/MPU 等の target 条件を検証する hook です。
