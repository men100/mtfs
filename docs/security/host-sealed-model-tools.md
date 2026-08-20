# Phase 4.1A Host sealed model tools

Phase 4.1Aはsealed model package v1のHost reference implementationである。実装は
`tools/sealed_model/`だけに配置し、OpenSSL依存を既存Host testやRA/ST buildへ伝播させない。
embedded productionの`sealed_blob`、`model_store`、hardware provider、provisioning、NPU loadは含まない。

## Buildとtest

基準環境はWSL2 Linux、CMake 3.16以上、C++17 compiler、OpenSSL 3.x development packageである。
Ubuntuでは`libssl-dev`が必要になる。独自AES/GCM fallbackはない。

```sh
cmake -S tools/sealed_model -B /tmp/mtfs-sealed-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/mtfs-sealed-build
ctest --test-dir /tmp/mtfs-sealed-build --output-on-failure
```

sanitizer構成:

```sh
cmake -S tools/sealed_model -B /tmp/mtfs-sealed-asan \
  -DCMAKE_BUILD_TYPE=Debug -DMTFS_SEALED_ENABLE_SANITIZERS=ON
cmake --build /tmp/mtfs-sealed-asan
ctest --test-dir /tmp/mtfs-sealed-asan --output-on-failure
```

全targetは`-Wall -Wextra -Wpedantic -Werror`を既定にする。build directoryはrepository外へ置く。
配布用Host CLIだけをbuildする場合は`-DBUILD_TESTING=OFF`を指定する。この構成ではtest executableや
vector pathを参照せず、固定test keyをbinaryへ組み込まない。test有効時もkeyはruntimeに専用pathから読む。

## CLI

fleet keyを生成する。raw key fileは正確に32 byteで、`key_id=1`、`key_version=1`固定である。

```sh
mtfs-keygen --output fleet.key
```

既存fileは既定で上書きしない。`--overwrite`を明示した場合だけ置換する。Linuxでは同じdirectoryの
mode `0600` temporary fileへ書き、flush、`fsync`、close後にcommitする。Windows nativeは対象外で、
Windows ACLをPOSIX permission相当として保証しない。鍵をconsoleへ表示しない。

model packageを生成する例:

```sh
mtfs-seal \
  --key fleet.key \
  --input model.bin \
  --output model.mtfs \
  --target-id 1 \
  --accelerator-id 1 \
  --model-format 1
```

`--model-id`を省略するとCSPRNGでUUID v4を生成して成功時に表示し、`--model-version`は1、
`--required-ram`は入力binary size、`--chunk-size`は65536を既定値とする。自動生成されたmodel IDは
secretではない。更新packageを同じ論理modelのversion 2として扱う場合は、最初に表示されたIDを記録し、
次のように同じ`--model-id`と新しい`--model-version`を明示する。

```sh
mtfs-seal \
  --key fleet.key \
  --input model-v2.bin \
  --output model-v2.mtfs \
  --model-id 00112233-4455-4677-8899-aabbccddeeff \
  --model-version 2 \
  --target-id 1 \
  --accelerator-id 1 \
  --model-format 1
```

runtime/model formatがmodel binaryより大きい連続destinationを必要とする場合だけ、
`--required-ram`で明示的に上書きする。4～64 KiBの別chunk sizeが必要な場合だけ`--chunk-size`を指定する。

`target-id`は個々のdevelopment board名ではなく、packageをloadできるtarget/runtime familyを表す。
たとえばRA8P1系とSTM32N657系は別targetとなるが、同一のload ABIを共有するboard派生は同じ値を
使用できる。`accelerator-id`はそのtarget上のCPU/NPU provider、`model-format`はproviderが受理する
runtime model形式を表す。これら3 fieldの具体的なenum値はPhase 4.0/4.1Aでは未定義であるため、
deploymentごとに一貫したregistryを定め、verifierにも期待値を指定しなければならない。

入力はchunk単位で処理する。toolはpackageごとに`K_model`、package ID、key nonce、payload nonce
prefixをOpenSSL CSPRNGで生成し、同じdirectoryのtemporary fileへsealする。close後にpackageを再度
認証・復号し、sourceと復号streamのSHA-256を比較してからだけfinal nameへcommitする。
production CLIに固定key/nonce optionはない。CSPRNGがnonce重複を発生させないことに依存し、外部catalogが
重複を発見した場合はそのpackageを配布せずfleet key運用を停止しなければならない。

EK-RA8P1のfleet固有KAT artifactを生成する場合:

```sh
mtfs-kat-package \
  --key fleet.key \
  --package MTFSKAT.MTF \
  --descriptor MTFSKAT.TXT
```

`MTFSKAT.MTF`は公開された5000-byte patternをpayloadとするPhase 4.1A形式のpackageであるが、
`K_model`、package ID、nonceは毎回CSPRNG生成し、envelopeは指定した実運用`fleet.key`で作る。
`MTFSKAT.TXT`はsize、公開pattern、package/payload SHA-256だけを含む非秘密のコピー確認用descriptorで、
鍵や復号に必要な追加情報を含まない。2ファイルをSD rootへコピーし、raw `fleet.key`はコピーしない。
既存出力は既定で置換せず、意図した再生成時だけ`--overwrite`を指定する。

認証だけを行う場合:

```sh
mtfs-verify --key fleet.key --input model.mtfs \
  --target-id 1 --accelerator-id 1 --model-format 1 --max-chunk-size 65536
```

policy optionを省略すると、そのfieldは任意の認証済み値を受理する。target/accelerator/model formatの
enum値はPhase 4.0で未定義のため、deployment verifierは期待値を指定する。認証前metadataは表示しない。

認証済みplaintextをfileへ復元するreference loader:

```sh
mtfs-unseal --key fleet.key --input model.mtfs --output recovered.bin
```

復元先もtemporary file、flush、`fsync`、close、commitの順で処理し、tag、I/O、crypto failureでは
partial plaintextとtemporary fileを削除する。既存出力の置換には`--overwrite`が必要である。

全CLIのexit statusは、0が成功、1が処理失敗、2がusage/argument errorである。wrong keyと暗号化byteの
破損は同じ簡潔なauthentication errorとして扱う。production CLIは鍵、復号plaintext、model内容を
consoleまたはlogへ出力しない。model ID、version、package IDなどの非secret識別子は、通常動作に必要な
範囲で表示してよい。nonce、tag、ciphertextのdumpは通常出力では行わず、test artifactまたは明示的な
診断機能に限定する。

## v1 codecとcleanup

readerは160-byte preamble、最大4096-byte canonical TLV、48-byte envelope、全chunk、期待file sizeを
payload処理前に検査する。manifestはfileから読んだ`preamble || metadata`そのものをAADに使い、fieldから
再serializeしない。empty payloadはADRどおり許可し、その場合だけ`chunk_count=0`とする。domain labelは
末尾NULを1 byteだけ含む固定arrayである。TLVのsemantic/policy判断はenvelope認証成功後にだけ行う。

memory loaderはdestination size検査をpayload I/O前に行い、この失敗ではdestinationを変更しない。
payload I/O開始後の任意の失敗では先頭`payload_plain_length` byteだけを`OPENSSL_cleanse`し、余剰領域を
変更しない。chunk plaintextはtag成功後だけdestination/fileへ渡す。key、scratch、hash bufferも全経路で
同じzeroization primitiveを通す。

## Test vector

`tools/sealed_model/tests/vectors/`はすべて **TEST ONLY - NOT FOR PRODUCTION** である。Python
`cryptography`の`AESGCM`でC++実装と独立して生成し、次を格納する。

- raw test `K_fleet`とexpected `K_model`
- canonical binary/hex manifestとcomplete package
- envelope ciphertext/tag
- chunkごとのnonce、AAD、ciphertext、tag、expected plaintext
- representative expected-failure mutation offset

`golden_vector.json`と`golden_manifest.hex`はC arrayへ機械変換できる。Phase 4.1B providerは同じvectorで
envelope unwrap、4/16/64 KiB GCM、短い最終chunk、tag failureを確認すること。合否条件は、全byte一致、
tag failure時にscratch外へplaintextを出さないこと、abort/close後にkey stateを再利用できないこと、
destinationの正確なzeroization範囲をtarget上でも満たすことである。test keyをproduction build、device、
provisioning artifactへ含めてはならない。

## Security limitations

v1はfleet key holder authenticity、confidentiality、integrityを提供するが、publisher identity、anti-rollback、
OTA、volume metadata保護、正規firmware/RAM侵害、同じfleet内deviceへのSD clone拒否を提供しない。
Host raw key fileの保護・backup・削除は利用者のkey vault運用に依存する。Host providerのsoftware handleは
hardware-backed key isolationを表さない。
