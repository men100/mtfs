# Common test cases

ブロック層、FatFs統合、OS適合、拡張機能の共通契約を検証するケースを配置します。RA/ST/ホストの各ランナーから適用可能なケースを利用します。

`test_fatfs_roundtrip` は渡されたvolume path上で既知patternの永続化を検証します。`test_fatfs_concurrent` はPOSIXホスト用で、2本のpthreadと独立した `FIL` を使って同一volumeの並行アクセスを検証します。どちらもdrive 0を固定せず、volume pathと8.3 filenameを固定長buffer内で検証して結合します。

`test_fatfs_lfn`は`MTFS_FF_USE_LFN=2`、`MTFS_FF_CODE_PAGE=437`でASCII長名のcreate/write/sync/stat/rename/readdir、
unmount/remount、SFN alias衝突、最大長、異常系、long directory、cleanupを共通検証します。
開始時に専用名が存在すれば上書きせず失敗します。LFN無効時は設定が0であることだけを確認し、
媒体上へ長名test objectを作りません。並行testはLFN有効時にtask/threadごとの別長名を使います。
試験名は`A-Z`、`a-z`、`0-9`、space、`-`、`_`、`.`を含みます。CP437はASCII互換ですが、
正式な試験範囲はこのASCII文字集合であり、CP437拡張文字は対象外です。
