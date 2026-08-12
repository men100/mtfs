# STM32 SDMMC block device

`mtfs_stm32_sdmmc_context_t` を静的確保し、`mtfs_stm32_sdmmc_context_init()` に `SD_HandleTypeDef *`、IRQ 番号、媒体検出、書込み保護、IDMA 利用可否を渡します。取得した `mtfs_block_device_t` は registry / FatFs disk bridge から通常の同期 Block Device として使用できます。`hsd2` や `SDMMC2_IRQn` はポートに固定していません。

転送経路:

- `use_idma=1`: `HAL_SD_ReadBlocks_DMA()` / `HAL_SD_WriteBlocks_DMA()`、SDMMC IRQ、HAL 完了 callback、T-Kernel event flag で同期化する既定経路。
- `use_idma=0`: 同じ API と mutex を保ち、`HAL_SD_ReadBlocks()` / `HAL_SD_WriteBlocks()` を使う polling fallback。
- 1 回の転送は最大 8 sector。単一・複数 block の開始回数、最大 block 数、IRQ/callback/timeout/abort は `diagnostics` で観測できる。

IDMA では context 内の 4096-byte bounce buffer だけを DMA 対象にします。buffer は 32-byte aligned かつ cache-line 完結で、write は copy 後 clean、read は転送前 clean/invalidate、完了後 invalidate してから user buffer へ copy します。FatFs が未整列 buffer を渡しても隣接 cache line を破壊しません。実際の配置領域は linker map と実機ログの両方で確認してください。

同期と復旧:

- FatFs volume mutex → Block Device 呼出し → SDMMC access mutex の順です。SDMMC mutex を保持したまま FatFs API を呼ばないでください。
- HAL callback/IRQ は mutex を取らず event flag のみを更新します。
- IDMA 完了待ちと card-transfer 状態待ちには独立 timeout があり、失敗時は abort して未初期化へ戻します。次回 initialize で復旧できます。
- deinit は IRQ、T-Kernel object、HAL、任意の HAL timebase を解放します。並行 I/O がない状態で呼んでください。

制約:

- HAL の global SD callback を handle 一致で dispatch するため、IDMA context は同時に 1 instance です。
- geometry は HAL card info から sector count/size を取得し、erase block は保守的に 1 sector、trim は unsupported です。
- card-detect / write-protect は target callback の能力に従います。hot-plug 中の I/O は保証せず、挿抜後は明示再初期化または再起動を前提にします。
- `idma_platform_ready` は RIF/MPU 等の target 条件を検証する hook です。
