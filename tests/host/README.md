# Host test runner

ホストポートで共通テストケースを実行するランナーです。実機投入前に、FatFsから共通Block Device APIを経由する基本I/Oと同一volumeへの並行アクセスを高速に回帰確認します。

基準環境はWSL2上のUbuntu 26.04 LTSです。ネイティブWindowsを含む他のホストOSは対象にしません。本物のUbuntuでも同じLinuxビルドとして動作する想定です。

必要なパッケージは `build-essential` と `cmake` です。Ninjaを使う場合のみ `ninja-build` も追加してください。

```sh
cmake -S tests/host -B /tmp/mtfs-host-build
cmake --build /tmp/mtfs-host-build
ctest --test-dir /tmp/mtfs-host-build --output-on-failure
```

ホストtargetは `MTFS_FF_FS_REENTRANT=1`、POSIX mutex適合層、`MTFS_FF_FS_NORTC=1` でビルドし、`Threads::Threads` を使用します。ランナーのreporter callbackだけが標準出力へ依存し、共通frameworkは `stdio` を必要としません。

テストは次を実行します。

- ファイルbacked Block DeviceとDisk I/Oブリッジの境界・read-only動作
- format、unmount/remount、既知patternのround-trip
- 2本のpthreadが別々の `FIL` で `TASKA.BIN` と `TASKB.BIN` を同時に書込み、join後に再マウントしてサイズと全内容を検証
- read-only Disk I/Oブリッジと `FF_FS_REENTRANT=0` FatFs構成のcompile-only target

テストイメージはランナーが `/tmp` に一意な名前で作成し、専用テストファイルとimageを試験終了時に削除します。既存ファイルや実機メディアをフォーマットすることはありません。
