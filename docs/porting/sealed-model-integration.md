# Sealed model統合ガイド

sealed modelは通常のFatFs/Block Device利用には不要なoptional機能です。microT-FSの公開設定
`MTFS_ENABLE_SEALED_MODEL`は既定で0であり、FatFs-only構成からwrapped-key、hardware crypto、
key-storeとvendor crypto headerへの依存を除外します。

## FatFs-only構成（既定）

設定を指定しないか、全translation unitで明示的に次を指定します。

```c
#define MTFS_ENABLE_SEALED_MODEL 0
```

この構成では次のmicroT-FS sourceを追加する必要はありません。

```text
src/extensions/security/wrapped_key/
src/ports/ra_fsp/crypto/
src/ports/stm32_cube/crypto/
```

RA FSPのRSIP/PSA/Mbed TLS crypto stack、STM32CubeのCRYP/RNG driver、NOR用XSPI/BSPも追加しません。
optional sourceを含む`src/`全体をIDEへ登録してもmicroT-FS source自体は空のtranslation unitとして
compileできますが、vendor sourceの選択はbuild systemの責務です。不要なvendor sourceをsource一覧へ
残すと最終imageからlinkerが除去してもclean build時間は短くなりません。

Host buildの`mtfs_sealed_model_disabled_compile` targetは、OpenSSL、FSP、STM32 HALのinclude pathを
与えずに全optional sourceとheaderをcompileし、このOFF契約を回帰確認します。

## 機能を有効にする

利用するtargetの全C translation unitへ次を指定します。

```c
#define MTFS_ENABLE_SEALED_MODEL 1
```

repositoryのRA/STM32 basic runnerと両key-provision applicationは、Debug/ReleaseのC compiler設定で
明示的に1を指定しています。Host package toolはtarget buildとは独立したCMake/OpenSSL programです。

共通のwrapped-key recordを使う場合は次を追加します。

```text
src/extensions/security/wrapped_key/mtfs_wrapped_key_record.c
src/extensions/security/wrapped_key/mtfs_wrapped_key_fatfs.c  # FatFs保存を使う場合
```

### EK-RA8P1 / RSIP-E50D

target adapterは次です。

```text
src/ports/ra_fsp/crypto/mtfs_ra8p1_ospi_key_store.c
```

参照basic projectのFSP 6.5.0設定に合わせ、RSIP-E50D Compatibility/plaintext key mode、
`r_rsip_e50d_key_injection`、`rm_psa_crypto`、PSA/Mbed TLS AES-GCM sourceと必要なinclude pathを追加します。
fleet key recordを置くEK-RA8P1 onboard OSPI NOR末尾8 KiB（offset `0x03FFE000`/
`0x03FFF000`）を他用途、firmware image、erase範囲から除外します。開発用provisioning手順は
[`apps/key-provision/targets/ek_ra8p1/README.md`](../../apps/key-provision/targets/ek_ra8p1/README.md)を
参照してください。

### STM32N6570-DK / SAES-DHUK

target providerとkey storeは次です。

```text
src/ports/stm32_cube/crypto/mtfs_stm32_saes.c
src/ports/stm32_cube/crypto/mtfs_stm32_nor_key_store.c
src/ports/stm32_cube/crypto/mtfs_stm32n6570_nor.c
src/ports/stm32_cube/common/mtfs_stm32_hal_timebase.c
```

STM32Cube FW_N6 V1.3.0のHAL CRYP/CRYPEx、RNG/RNGEx、XSPI driver、STM32N6570-DK XSPI BSP、
MX66UW1G45G componentと各include pathを追加します。providerは64 KiB input、64 KiB private
authenticated-output、4 KiB AADのstatic workspaceを現在使用するため、application固有bufferとは別に
link mapとRAM配置を確認します。MX66UW1G45G末尾8 KiB（offset `0x07FFE000`/`0x07FFF000`）を
firmware imageとerase範囲から除外します。開発用provisioning手順は
[`apps/key-provision/targets/stm32n6570_dk/README.md`](../../apps/key-provision/targets/stm32n6570_dk/README.md)を
参照してください。

## 運用と確認

- raw fleet keyは通常applicationやSDへ置かず、専用provisionerの一時RAMだけへ入力します。
- whole-chip eraseはRA/STの予約slotも消去するため、trusted環境で再provisioningします。
- target packageはHost toolで生成し、fleet固有の`MTFSTEST.MTF`をSDへ配置します。
- basic runnerの`crypto-info`、`crypto-consistency`、`crypto-negative`、`crypto-package-test`でprovider、
  OpenSSL相互運用、改ざん拒否、zeroization、SD/FatFs cleanupを確認します。
- 製品化ではdebug lock、lifecycle、tamper、anti-rollback、remote provisioningを別途設計します。

providerの実装境界と実機受入結果は
[`docs/security/secure-ai-storage.md`](../security/secure-ai-storage.md)、RAの詳細は
[`docs/security/ek-ra8p1-phase-4.1b-ra2.md`](../security/ek-ra8p1-phase-4.1b-ra2.md)、STM32の詳細は
[`docs/security/stm32n657-saes-dhuk-spike.md`](../security/stm32n657-saes-dhuk-spike.md)を参照してください。
