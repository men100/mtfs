# Block Device portの実装契約

`mtfs_block_device_t`はFatFsの型を含まない同期Block Deviceです。portは静的確保可能な
context、operation table、capabilityを組み立て、vendor/HALの結果を`mtfs_error_t`へ
変換します。

## 型とgeometry

`mtfs_lba_t`は`uint64_t`です。`sector_size`は1 logical sectorのbyte数、
`sector_count`は利用可能なlogical sector総数、`erase_block_size`はsector単位の
erase block sizeです。3値はすべて0以外でなければ共通wrapperが
`MTFS_ERROR_INVALID_ARGUMENT`を返します。

有効な要求範囲は`count > 0`かつ`lba < sector_count`かつ
`count <= sector_count - lba`です。減算後に比較するため`lba + count`のoverflowを
避けています。共通`read`/`write`/`trim` wrapperはNULL buffer、count 0、不正範囲を
portへ渡しません。port固有operationも直接呼ばれた場合に備えて検査します。

現在のFatFs設定は`FF_MIN_SS=FF_MAX_SS=512`です。したがって新規portを現設定で
FatFsへ接続する場合、geometryのsector sizeは512である必要があります。

## operation

| operation | 必須 | 契約 |
|---|---:|---|
| `initialize` | 必須 | 媒体/HALを利用可能にし、成功時だけINITIALIZED状態にする。失敗時は成功状態を残さない。再呼出しで復旧できる設計にする。 |
| `status` | 必須 | 現在の状態bitを返す。出力pointerはNULL不可。状態を確認できた場合は`MTFS_OK`、媒体不在等をerrorでも表せる。 |
| `read` | 必須 | `count` sectorを`buffer`へ同期的に読み、完了後にreturnする。部分成功を表すfieldはないため、全要求を完了できなければerrorを返す。 |
| `write` | read-only以外で必須 | `count` sectorを同期的に書く。write protectは`MTFS_ERROR_WRITE_PROTECTED`。 |
| `sync` | 必須 | pending writeが媒体へ到達するまで待つ。既に同期済みのportは状態確認後`MTFS_OK`を返せる。 |
| `get_geometry` | 必須 | 有効な3つのgeometry値を返す。容量未確定なら`MTFS_ERROR_NOT_READY`。 |
| `trim` | 任意 | capabilityがあるdeviceだけ実装する。範囲は`lba`から`count` sector。未対応deviceはcapabilityを立てず、共通wrapperは`MTFS_ERROR_NOT_SUPPORTED`を返す。 |

`mtfs_block_device_is_valid()`は`initialize`、`status`、`read`、`sync`、
`get_geometry`を必須とします。read-only capabilityがない場合は`write`も必須です。
TRIM capabilityを立てた場合は`trim`も必須です。未知のcapability bitは無効です。

## statusとcapability

- `MTFS_BLOCK_STATUS_INITIALIZED`: 現在I/O可能な論理初期化状態。
- `MTFS_BLOCK_STATUS_MEDIA_PRESENT`: 媒体が存在することを現在確認できる状態。
- `MTFS_BLOCK_STATUS_WRITE_PROTECTED`: 現在書込み禁止。read-only capabilityがある
  deviceでは共通wrapperもこのbitを追加する。
- `MTFS_BLOCK_CAPABILITY_READ_ONLY`: device設計上write不可。`write` operationはNULLで
  よく、共通wrapperはwriteを拒否する。
- `MTFS_BLOCK_CAPABILITY_TRIM`: trimを実装し、呼出しを受け付ける。

状態は固定能力ではありません。たとえば取外し可能媒体はMEDIA_PRESENTを落とし、
初期化や転送の失敗でI/O継続を保証できない場合はINITIALIZEDを落とします。

## error変換

| `mtfs_error_t` | portで使う状況 |
|---|---|
| `MTFS_ERROR_INVALID_ARGUMENT` | NULL、0 count、無効設定など呼出し契約違反 |
| `MTFS_ERROR_NOT_SUPPORTED` | 未対応command、sector size、媒体形式など |
| `MTFS_ERROR_IO` | HAL/媒体のI/O失敗で、より具体的な分類がない場合 |
| `MTFS_ERROR_NOT_READY` | 未初期化、busy、timeout、復旧待ち |
| `MTFS_ERROR_NO_MEDIA` | card detectまたは初期化応答で媒体不在を確認 |
| `MTFS_ERROR_WRITE_PROTECTED` | 動的または固定の書込み禁止 |
| `MTFS_ERROR_OUT_OF_RANGE` | LBA/count、容量、HALのaddress幅を超える要求 |
| `MTFS_ERROR_ALREADY_EXISTS` | 既存instance/registryとの競合 |
| `MTFS_ERROR_NOT_FOUND` | registryにpdrvがない場合など |

Disk I/O bridgeはこれらを`RES_OK`、`RES_WRPRT`、`RES_NOTRDY`、`RES_PARERR`、
`RES_ERROR`へ変換します。portから`DSTATUS`、`DRESULT`、`BYTE`、`LBA_t`などFatFs型を
返してはいけません。

## 失敗、再initialize、deinit

共通層はportの状態を推測または書き換えません。initialize失敗後、または転送失敗後の
INITIALIZED状態はportの責務です。継続可能性が保証できない失敗では未初期化へ戻し、
次の`initialize`でHAL/deviceを既知状態へ戻します。STM32 SDMMC portはtimeout/errorで
abortし、次回initializeでHAL deinit/initを行います。RA SPI portはinitialize開始時に
状態を初期化しますが、全ての転送errorで自動的に未初期化へ戻るわけではありません。

deinitはoperation tableにありません。IRQ、event flag、mutex、HAL handle、device
registrationなどport固有資源が異なるため、各portの`*_context_deinit()`を、unmountと
registry解除の後、並行I/Oがない状態で呼びます。

## context設計

contextは呼出し側がBSS等に静的確保できる具体型にします。operation中に必要なHAL
handle、状態、geometry、mutex/event flag ID、診断値、DMA bufferを保持し、通常経路で
heap allocationを要求しない設計が既存portの共通形です。operation tableは`static
const`とし、`device.context`だけからinstanceへ到達できるようにします。IRQから参照する
global dispatchが必要なら、instance数制約とdeinit時の解除を明記します。

registryは固定配列で、登録/解除自体のmutexを持ちません。I/O task開始前に登録し、全
利用task停止後に解除します。

## 実装チェックリスト

- [ ] `mtfs_block_device_t`と全必須operationを実装した
- [ ] contextとbufferを静的確保できる
- [ ] count 0、NULL、LBA範囲、HAL address幅を検査した
- [ ] geometryの3値をsector単位で返す
- [ ] initialize失敗時の状態を定義した
- [ ] 転送失敗後に継続可能か、再initializeが必要かを定義した
- [ ] read-only/write protect/trim capabilityがoperationと一致する
- [ ] vendor errorを`mtfs_error_t`へ変換した
- [ ] FatFs固有型をportへ持ち込んでいない
- [ ] deinitをport固有APIとして用意した

