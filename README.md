# microT-FS

microT-FS は、microT-Kernel 3.0 向けに FatFs を最適化・拡張するストレージ基盤です。組み込み用途で扱いやすいブロック I/O、OS 適合層、ボード固有ポートを一つの配布単位にまとめます。

## 対象環境

- Renesas EK-RA8P1（RA FSP）
- STMicroelectronics STM32N6570-DK（STM32Cube）
- ホスト環境（単体テストおよび開発補助）

将来は TrustZone を利用したセキュアストレージ、暗号化、完全性検証、監査、AI 向け I/O テレメトリ、異常アクセス検知、ストレージ最適化、およびコンテスト向け統合デモへ発展させます。

## 利用方法と配布単位

本番利用に必要なコードは、FatFs の microT-FS 管理版と RA/ST 固有コードを含めて、すべて `src/` 配下に置きます。利用者は原則として `src/` を組み込みプロジェクトへコピーし、`mtfs_config.h` と対象ポートのビルド設定を調整します。

現在は初期リポジトリ構成のみであり、FatFs 本体や microT-FS の C 実装はまだ含みません。

## ディレクトリ構成

```text
src/                  本番用ソース一式（ライブラリの配布単位）
  core/               ファイルシステム中核
  fatfs/              microT-FS が管理・改良する FatFs
  block/              ブロックデバイス抽象化とフィルタ
  os/microtkernel/    microT-Kernel 3.0 適合層
  ports/              RA FSP、STM32Cube、ホスト向けポート
  extensions/         セキュリティおよび AI 拡張
tests/                共通テストと構成別テストランナー
apps/                 統合デモおよびサンプルアプリケーション
external/             本番配布物に含めない外部依存物
docs/                 設計、移植、利用者向け文書
assets/               文書やデモで使用する素材
tools/                開発、検証、生成補助ツール
mtk3_bsp2/            microT-Kernel 3.0（submodule）
```

FatFs は外部 submodule として扱わず、microT-FS の中核として `src/fatfs/` で管理します。上流のライセンス、元バージョン、取得元、配布 ZIP の SHA-256、および独自変更は同ディレクトリの文書で追跡します。

`mtk3_bsp2/` は TRON Forum の `mtk3_bsp2` を submodule として参照します。既存の clone では `git submodule update --init --recursive` を実行してください。

`tron2026_work` は成果や知見の参照元としてのみ利用し、コードや IDE プロジェクトをそのままコピーせず、microT-FS の設計に合わせて再構築します。

## ライセンス

リポジトリ全体のライセンスは [LICENSE](LICENSE) を参照してください。将来取り込む FatFs には上流ライセンスが適用されるため、詳細は `src/fatfs/` の来歴文書で明示します。
