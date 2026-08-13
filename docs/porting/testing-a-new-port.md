# 新規target runnerの作り方

実機testは「1 test case = 1 IDE project」ではなく、同じSDK/clock/FatFs設定で動くtest群を
1つのrunnerから順に実行します。

## 責務の分離

- `tests/common/framework/`: vendorや`stdio`に依存しないreporter契約とcheck集計
- `tests/common/cases/`: Block Device/FatFsの共通test case
- `tests/common/microtkernel/`: microT-Kernel上の共通並行test
- `tests/targets/<board>/<runner>/`: IDE project、生成設定、target startup、port config、
  IRQ、reporter、実行順

target固有のtask、IRQ、GPIO、RIF、UART/`tm_printf()`処理はtarget側へ置きます。共通caseへ
vendor HAL headerやboard pinを追加しません。生成projectはmicroT-FS本体と共通testを相対
linked resourceで参照し、sourceを複製しません。

## runnerの基本順序

同じbuild設定の1 roundで次を順に実行し、error経路でも逆順cleanupします。

1. 静的contextへport固有初期化を行う。
2. `mtfs_block_device_t`を取得する。
3. `mtfs_block_initialize()`を実行する。
4. `mtfs_block_registry_register(pdrv, device)`で登録する。
5. `mtfs_block_status()`と`mtfs_block_get_geometry()`を確認する。
6. sector 0をraw readする。
7. 1 sector readと複数sector readを行い、重なる範囲を比較する。
8. `test_fatfs_roundtrip`でmount/write/sync/read/unmountを確認する。
9. 独立した`FIL`を持つ2 taskで同一volumeへ並行accessする。
10. unmount/remount後に内容を再検証する。
11. runnerが作成したtest fileを削除する。
12. unmount、registry解除、task/event flag、port contextを解放する。

Block Device context初期化と媒体initializeは別段階です。どちらが失敗したかをreportし、
作成済み資源だけをcleanupします。reporterを複数taskから呼ぶ場合はtarget側で直列化するか、
workerは結果だけを保存してcoordinatorがreportします。

## 媒体を壊さない条件

- runnerから`f_mkfs()`を呼ばず、事前format済みのtest媒体を使う。
- raw sector writeは専用scratch領域の所有を証明できない限り行わない。
- sector 0末尾の`0x55AA`は情報表示に留め、合否条件にしない。
- 重要dataのない媒体を使い、runnerが作るfilenameを事前に文書化する。
- task間で同じ`FIL` objectを共有しない。
- 実機未実施のprofileや転送経路をPASSと記録しない。

## profile

既存runnerは同じcase集合の反復数を変えます。

| profile | 既存runnerのround数 | 用途 |
|---|---:|---|
| smoke | 1 | bring-up、媒体なしの有限timeout、基本経路の短時間確認 |
| normal | 10 | 通常の回帰確認 |
| stress | 100 | 同じbuild設定での反復安定性確認 |

profile名だけで試験内容を推測せず、実行したbuild define、port経路、媒体、round数、最終logを
記録します。STM32ではIDMAとpollingを別buildとして確認します。DMA経路ではIRQ/Rx/Tx、
single/multi block、timeout/abort counterもassertし、pollingでは複数sector要求が内部で
1 sector転送へ分割された診断値を確認します。

## Host regression

実機projectを変更しても、共通Block Device/Disk I/O/FatFsの退行はHostで先に検出します。

```sh
cmake -S tests/host -B /tmp/mtfs-host-build
cmake --build /tmp/mtfs-host-build
ctest --test-dir /tmp/mtfs-host-build --output-on-failure
```

build directoryはGit管理外に置きます。Host PASSは実機HAL、IRQ、DMA、配線の確認を代替しません。

## 新規port/runnerチェックリスト

- [ ] 対象deviceとSPI/SDMMC/その他の接続方式を決めた
- [ ] `mtfs_block_device_t`を契約どおり実装した
- [ ] contextと大きなbufferを静的確保した
- [ ] geometryとerror変換を定義した
- [ ] FatFs/portの排他とIRQ contextを確認した
- [ ] HAL timebaseと全timeoutを確認した
- [ ] DMA対象RAM、cache maintenance、bus master属性を確認した
- [ ] generated codeと利用者管理codeの境界を決めた
- [ ] linked resource/include pathがrepository相対である
- [ ] build生成物をignoreした
- [ ] 公開文書/projectにlocal絶対pathがない
- [ ] Host testsを実行した
- [ ] target smokeを実行した
- [ ] single/multi-sector raw readを実行した
- [ ] FatFs roundtripと2 task並行testを実行した
- [ ] unmount/remount後を検証した
- [ ] timeout/abort/再initializeを確認した
- [ ] registry、task、同期object、contextを解放した
- [ ] 実施したprofileと未実施項目を区別して記録した
- [ ] 現在の制限事項を記録した
