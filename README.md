# microT-FS

microT-FSは、microT-Kernel 3.0向けにFatFsを統合・拡張する組み込みストレージ基盤です。Block Device API、OS適合層、board固有portをまとめて提供します。

## 対象target

- Renesas EK-RA8P1
- STMicroelectronics STM32N6570-DK
- Host test環境

組み込み用の配布単位は `src/` です。

## 構成

`MTFS_ENABLE_SEALED_MODEL`の既定値は`0`です。FatFs-only構成を標準とし、必要に応じてsealed model機能を有効にできます。

このrepositoryは現在、release documentationの再構成前です。

## Submodule

microT-Kernel 3.0 BSPを取得するには、repositoryのrootで次を実行します。

```console
git submodule update --init --recursive
```

## License

repository全体のlicenseは [LICENSE](LICENSE) を参照してください。収録したFatFsには上流licenseが適用されます。来歴と変更情報は `src/fatfs/UPSTREAM.md`、`src/fatfs/CHANGES.mtfs.md`にあります。
