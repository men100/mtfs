# Common test cases

ブロック層、FatFs統合、OS適合、拡張機能の共通契約を検証するケースを配置します。RA/ST/ホストの各ランナーから適用可能なケースを利用します。

`test_fatfs_roundtrip` は渡されたvolume path上で既知patternの永続化を検証します。`test_fatfs_concurrent` はPOSIXホスト用で、2本のpthreadと独立した `FIL` を使って同一volumeの並行アクセスを検証します。どちらもdrive 0を固定せず、volume pathと8.3 filenameを固定長buffer内で検証して結合します。
