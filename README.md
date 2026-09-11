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

組み込みtest projectは `tests/targets/ek_ra8p1/` と
`tests/targets/stm32n6570_dk/` にあります。前者はe² studio project、後者は
STM32CubeIDEのmulti-project（Appli/FSBL）です。import/build手順とpin設定の
source of truthは各directoryのREADMEを参照してください。

console applicationでは `log-level [off|error|info|debug]` で実行時ログレベルを
確認・変更できます（既定は`info`、reset後の永続化なし）。`help`は一般commandと
group一覧、`help <group>`は該当group、`help all`は全commandを表示します。

## Submodule

microT-Kernel 3.0 BSPを取得するには、repositoryのrootで次を実行します。

```console
git submodule update --init --recursive
```

## License

repository全体のlicenseは [LICENSE](LICENSE) を参照してください。収録したFatFsには上流licenseが適用されます。来歴と変更情報は `src/fatfs/UPSTREAM.md`、`src/fatfs/CHANGES.mtfs.md`にあります。
