# FatFs in microT-FS

ここには elm-chan 配布 ZIP を基にした FatFs と、microT-FS 向けの改良を配置します。FatFs は submodule や `external/` の依存物ではなく、`src/` とともに配布する中核コードです。

初期構成の時点では FatFs 本体をまだ取り込んでいません。取り込み時は次を同じコミットで記録します。

- `UPSTREAM.md`: 元バージョン、取得元 URL、取得日、ZIP ファイル名、SHA-256、上流ライセンス
- `CHANGES.mtfs.md`: 上流からの独自変更と、その理由・影響
- 上流配布物に含まれるライセンス文書またはライセンス表示

上流ファイルを更新するときも、再現可能な来歴と差分を維持します。
