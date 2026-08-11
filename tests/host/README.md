# Host test runner

ホストポートで共通テストケースを実行するランナーです。実機投入前に、FatFsから共通Block Device APIを経由する基本I/Oを高速に回帰確認します。

基準環境はWSL2上のUbuntu 26.04 LTSです。ネイティブWindowsを含む他のホストOSは対象にしません。本物のUbuntuでも同じLinuxビルドとして動作する想定です。

必要なパッケージは `build-essential` と `cmake` です。Ninjaを使う場合のみ `ninja-build` も追加してください。

```sh
cmake -S tests/host -B /tmp/mtfs-host-build
cmake --build /tmp/mtfs-host-build
ctest --test-dir /tmp/mtfs-host-build --output-on-failure
```

テストイメージはランナーが `/tmp` に一意な名前で作成し、試験終了時に削除します。既存ファイルや実機メディアをフォーマットすることはありません。
