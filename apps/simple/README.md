# Simple FatFs application

microT-FSを通常のFatFsとして使う最小サンプルです。FAT12/FAT16/FAT32で事前formatしたSDカードに対して、
次の処理を起動時に1回だけ実行します。

1. `0:`をmount
2. 8.3形式の`0:/HELLO.TXT`を作成して短い文字列を書込み
3. close後に再openし、全内容を読戻して比較
4. `f_unlink()`で`HELLO.TXT`を削除
5. `0:`をunmount

成功時はSDカードへファイルを残しません。途中で失敗した場合も、close、削除、unmountを可能な範囲で
実行します。

## 構成

- `MTFS_ENABLE_SEALED_MODEL`: 未指定（既定値0）
- `MTFS_ENABLE_DIAGNOSTICS=0`
- `MTFS_FF_USE_LFN`: 未指定（既定値0、8.3 filenameのみ）
- `MTFS_FF_FS_REENTRANT`: 未指定（既定値0、単一application task）
- `MTFS_FF_FS_NORTC`: 未指定（既定値1、固定timestamp）
- mkfs、exFAT、find、fast seek、relative path、multi-partition、trim、label、string I/Oなどの追加FatFs機能: OFF
- console、test framework、benchmark、RTC、wrapped-key、crypto provider: source一覧に含めない

`f_unlink()`を使うため、基本APIを削る`FF_FS_MINIMIZE`だけは0のままです。LFNや上記のoptional
featureを有効にする意味ではありません。

`MTFS_ENABLE_SEALED_MODEL=0`をprojectへ明示していない点も、このサンプルの意図です。
FatFs-only利用者が何も設定しなくてもcrypto header/libraryへ依存しないことを示します。

## Targets

- `targets/ek_ra8p1`: e2 studio / FSP 6.5.0、Pmod MicroSDのSPI接続
- `targets/stm32n6570_dk`: STM32CubeIDE、onboard microSDのSDMMC2接続

各projectをIDEへimportしてDebugまたはReleaseをbuildします。UART/T-Monitorには各段階のPASS/FAILが
表示され、最終行は`[simple] application PASS`になります。
