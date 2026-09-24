# Sealed Model / Security guide

## Threat modelと保証範囲

sealed modelは、盗難・複製・改ざんされたremovable SD上のmodel packageを想定し、modelの機密性、改ざん検出、およびtarget/accelerator policyの検証を提供します。

一方、完全に侵害されたfirmware、実行中RAMの解析、rollback防止、OTA更新、remote attestationは保証範囲外です。初版ではTrustZoneによる分離は行わず、application側でsecure configurationを行った環境で実行します。

`K_fleet`は、device fleetへ安全にprovisionするAES-256 keyです。host toolはpackageごとに`K_model`を生成し、`K_fleet`を使ってenvelopeとして保護します。

payloadは4/8/16/32/64 KiB単位のchunkに分割し、AES-256-GCMで処理します。既定のchunk sizeは64 KiBです。envelopeと各chunkのAEAD tagを検証し、認証に成功した後でplaintextを利用します。

target側では、`K_fleet`をhardware-backed wrapped keyとしてRA OSPIまたはST external NORのdual slotへ保存し、通常のfirmware内にはraw keyを埋め込みません。

`model_store`は、認証済みmetadataに対してrequired RAM、target、accelerator、formatを検証し、呼び出し側が用意したstatic arenaへloadします。失敗時およびclose時には、key、AAD、ciphertext/plaintext用のwork areaをzeroizeします。

## Host tools

[Getting Started](getting-started.md#hostで試してみる)の手順でbuildすると、次のtoolを利用できます。

```console
mtfs-keygen --output fleet.key
mtfs-seal --key fleet.key --input model.bundle --output MODEL.MTF \
  --target-id <id> --accelerator-id <id> --model-format <id>
mtfs-verify --key fleet.key --input MODEL.MTF \
  --target-id <id> --accelerator-id <id> --model-format <id>
mtfs-unseal --key fleet.key --input MODEL.MTF --output recovered.bundle
mtfs-test-package --key fleet.key --package MTFSTEST.MTF \
  --descriptor MTFSTEST.TXT
```

| tool | 入力 | 出力/役割 |
| --- | --- | --- |
| `mtfs-keygen` | 出力先path | OpenSSLのCSPRNGで256-bit `K_fleet`を生成し、exact 32-byte binary key fileとして保存 |
| `mtfs-seal` | `K_fleet` file、plaintext model bundle、target/accelerator/format policy | packageごとの`K_model`とmodel IDを生成し、chunked AEAD packageを作成してself-verify |
| `mtfs-verify` | `K_fleet` file、sealed package、optional policy | package全体のformat/AEAD authenticationと指定policyを検証。plaintext fileは出力しない |
| `mtfs-unseal` | `K_fleet` file、sealed package、出力先 | 認証後のplaintext bundleをHost上へ取り出す。通常のtarget deploymentには不要 |
| `mtfs-test-package` | `K_fleet` file、package/descriptor出力先 | target test用のsynthetic payloadを持つ`MTFSTEST.MTF`とnon-secret descriptorを生成。production model用ではない |

`mtfs-seal`の`--model-id`を省略するとUUIDを生成し、`--model-version`の既定値は1です。
`--required-ram`、4～64 KiBの`--chunk-size`、target/accelerator/format IDは、target側の
policyと一致させます。`mtfs-verify`と`mtfs-unseal`にも期待するpolicyを指定すると、別target向け
packageの取り違えをHost側で検出できます。

現行Host toolがpackageへ記録するfleet key ID/versionは`1/1`固定で、CLIから変更できません。`apps/key-provision`も初回登録とreplacementの双方でID/version `1/1`を維持し、device-localなrecord generationだけを増やします。[replacementと非atomic deploymentの手順](applications.md#単一fleet-keyの明示的なreplacement)を確認してください。

複数key versionの同時保持、versioned key rotation、packageとdevice keyのatomic deployment、automatic rollback、anti-rollback、revocation、OTA key rotationは初版の保証範囲外です。旧`update-xmodem`は案内専用であり、keyを変更しません。

既存のoutput fileは既定では上書きしません。上書きする場合のみ`--overwrite`を明示的に指定します。

toolは同じdirectory内のtemporary fileへ書き込み、処理が完了した時点でatomicに置き換えます。key、model key、plaintextをconsoleへ出力することはありません。

CLIはraw keyのhex文字列を受け取らず、`--key`にはkey fileのpathを指定します。production key
fileは許可したOS accountだけが読めるdirectoryで管理し、key内容をcommand line、shell
history、console、CI logへ出力しないでください。`mtfs-unseal`の出力はplaintextなので、出力先の
access control、使用後の保持/廃棄方針も別途定めます。

`K_fleet`をtargetへ初回登録または明示的にreplacementする手順は、
[`apps/key-provision`](applications.md#appskey-provision)を参照してください。provisionerは
信頼できるlocal UART/XMODEM経路からkeyを受信してhardware-backed storageへwrapしますが、
Host側のkey生成、保管、backup、配布経路全体を提供するものではありません。

## 公開test key

> **TEST/DEMO KEY - PUBLIC AND NOT SECRET - DO NOT USE IN PRODUCTION**

[`fleet_test.key`](../tools/sealed_model/tests/vectors/fleet_test.key)は、公開test vectorとRA reference packageを再現するためだけに使用するdemo keyです。秘密情報として扱う必要はありませんが、production用途には使用しないでください。

production keyのentropy source、生成、保管、backup、deviceへの安全な配布、replacement、廃棄は利用者の責務です。正式対応するのは単一fleet keyの明示的なreplacementと手動rollbackであり、versioned rotationではありません。

[key-provision application](applications.md#appskey-provision)はdevice側の登録mechanismであり、
production向けkey-management/secure-room運用/remote provisioningを一式提供するsolutionでは
ありません。

設計上のdata flowについては[sealed model architecture](architecture/sealed-model.md)、artifactとthird-party softwareの扱いについては[licenses](licenses.md)を参照してください。
