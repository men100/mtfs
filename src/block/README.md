# Block layer

FatFs と物理メディアの間に置く、プラットフォーム非依存のブロックデバイス抽象化です。

- `mtfs_block_device.*`: 同期型の複数セクタI/O、状態、geometry、sync、optional trimを提供します。
- `mtfs_block_diagnostics.*`: version/size/validity付きのcached numeric snapshotとcounter resetを提供します。
- `mtfs_block_registry.*`: physical drive番号をキーに、デバイスを設定可能な固定長配列へ登録します。

この層はFatFsの型、`DSTATUS`、`DRESULT`に依存しません。登録はタスク開始前に行う前提で、レジストリ自体の排他制御は行いません。

診断契約、counterの単位、snapshot整合性、port実装方法は `../../docs/diagnostics.md` を参照してください。
