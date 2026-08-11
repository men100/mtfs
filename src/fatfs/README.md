# FatFs in microT-FS

このディレクトリには、FatFs R0.16 に公式 patch-1、公式 patch-2 の順で適用したベースラインを収録しています。現在の最終状態は `R0.16 w/patch 2` です。

FatFs は submodule や `external/` の外部依存物ではなく、microT-FS が管理・改良する中核コードとして `src/fatfs/` に含めます。

- 上流バージョン、取得元、ハッシュ、取り込み手順は `UPSTREAM.md` で管理します。
- microT-FS 独自変更とその理由・影響は `CHANGES.mtfs.md` で管理します。
- 上流ライセンスは `LICENSE.txt` を参照してください。

上流の `source/diskio.c` はサンプル／雛形のため収録していません。ディスクI/O実装は後続作業でmicroT-FS共通の `mtfs_diskio.c` として提供します。
