# Sealed-model Host tools

Linux/WSL2でのbuild方法、CLIの使い方、test/demo keyとproduction keyの使い分けについては、[`docs/security.md`](../../docs/security.md)を参照してください。

Host packageのfleet key ID/versionは常に`1/1`です。device側の単一fleet keyをreplacementする場合もID/versionは変えず、new keyでpackageを作り直します。key/packageの非atomic切替と手動rollbackは[`apps/key-provision`](../../docs/applications.md#単一fleet-keyの明示的なreplacement)を参照してください。

STM32N6570-DK向けのaccepted Neural-ART runtimeからbundle、optional seal、verify、unsealまでを実行するreference workflowは[Storage Sentinel guide](../../docs/storage-sentinel.md#stm32n6570-dk用sentinelmtfの生成)に記載しています。
