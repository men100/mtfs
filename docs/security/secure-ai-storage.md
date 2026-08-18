# セキュアAIストレージ・アーキテクチャ

この文書はPhase 4.0の設計成果であり、実装済みAPIの説明ではない。確定判断は
[`ADR 0003`](../adr/0003-secure-ai-storage.md)に従う。Phase 4.0ではsource、target project、
submodule、鍵materialを変更しない。

## 1. アーキテクチャ境界

```text
application
  +-- model_store -------------------- model識別 / NPU metadata / load契約
  |      +-- sealed_blob ------------- binary format / 境界検査 / AEAD / zeroization
  |             +-- crypto_provider -- opaque keyと暗号operationの境界
  |                    +-- RA RSIP-E50D provider
  |                    +-- STM32 SAES provider
  |                    +-- Host test provider
  +-- secure_media_sink -------------- 変換後commit
  +-- storage_sentinel --------------- diagnostics -> feature -> 状態/score
  +-- evidence_log ------------------- hash chain + device署名
```

将来TrustZone splitが必要になった場合はproviderだけを次へ置き換える。

```text
sealed_blob -> crypto_provider -> TrustZone NSC client -> Secure crypto service
```

初版のRA providerはflat application、STM32 providerは単一FullSecure applicationの一部である。
provider境界はsoftware architecture上の責務分離であり、現在のbuild内の攻撃者に対するsecurity
boundaryではない。

## 2. セキュリティ目標と対象外

### 目標

- SDだけを取得した攻撃者に対するmodel confidentiality
- header、metadata、key envelope、全payload chunkのintegrity
- `K_fleet` holderが作成したpackageであることの確認
- device固有wrapped formでの`K_fleet`保存
- raw keyをsealed/model APIへ公開しないprovider ABI
- 破損時に未認証または不完全なmodelをNPUへ渡さないload contract
- evidence署名鍵をmodel鍵階層から分離すること

### 対象外と制限

- model publisherの非対称identityは初版では確認しない。
- 同じfleet内の正規deviceへのSD cloneは拒否しない。
- packageにはmonotonic counterがなく、古い正規packageを拒否しない。
- FullSecure/flatの正規applicationから鍵利用を隔離しない。
- `K_fleet` holderまたは侵害されたfleet memberによるpackage作成を防がない。
- volume、directory entry、filename、free-space情報を暗号化しない。

## 3. 鍵階層とライフサイクル

### 3.1 鍵の役割

| 鍵 | 範囲 | 生成方法 | 永続保存 | 用途 |
|---|---|---|---|---|
| `K_fleet` | user/product fleet | `mtfs-keygen`、OS CSPRNG | Hostのkey vaultとdevice固有のHUK/DHUK-wrapped blob | `K_model` envelope専用 |
| `K_model` | model package 1個 | `mtfs-seal`、OS CSPRNG | SD上のAES-GCM envelope。seal完了後にHost上のraw値を破棄 | payload chunkのAEAD専用 |
| `K_evidence_priv` | device 1台 | device hardware RNG/provider | device固有のwrapped private-key blob | evidence record署名専用 |
| `K_evidence_pub` | device 1台、公開情報 | key pair生成時に導出 | Host verifier registry、および必要に応じてdevice metadata | offline検証 |

domain separationは構造で強制する。各鍵は別用途のoperationでは受理しない。key handleは用途
（`WRAP_MODEL_KEY`、`MODEL_PAYLOAD`、`EVIDENCE_SIGN`）を保持し、providerは用途が一致しない
呼出しを拒否する。

### 3.2 `K_fleet`のライフサイクル

1. `mtfs-keygen`はHost OSのCSPRNGで32 byteを生成する。明示的に選択され、access controlされた
   出力先だけへ書き込み、stdoutやlogには鍵を出力しない。
2. userは`key_id=1`、`key_version=1`を割り当てる。v1が扱うactive keyは1個だけとする。
3. provisioningではtargetを認証し、`K_fleet`をdevice固有のprovider blobへ変換する。
   - RA production経路では、Renesas UFPK/W-UFPK secure injectionとRSIP-E50D protected key形式を
     優先する。開発専用のplaintext injection経路ではFSP InitialKeyWrap APIを呼出してもよいが、
     trusted provisioning RAMへ入力鍵が現れるため、production profileからcompile outする。
   - STM32開発経路では、device上のSAESが入力鍵をDHUKでwrapする。production provisioningと
     lifecycle/HDPL policyは製品ごとの判断とし、Phase 4.0ではOTP programmingもlifecycle変更も行わない。
4. provider blob、algorithm/version、非secretのchecksumをtarget管理下のnonvolatile storageへ保存する。
   RAでは内部Data Flash、STM32N657ではDHUKで保護したblobをboard上のexternal flash等へ保存する候補がある。
   removable SDを唯一の保存先にはしない。
5. boot時または利用時にproviderがblob構造を検証し、opaqueな`K_fleet` handleをopenする。
   HUK/DHUKはhardware内部で選択し、application codeから読み出さない。
6. close時またはerror時には、providerがhardware stateを無効化し、一時的なcontrol/key-import bufferを
   zeroizeする。

初版のdevice recordは概念上、次のfieldを持つ。

```text
magic | record_version | provider_id | key_id | key_version |
wrapped_blob_length | wrapped_blob | checksum
```

checksumは保存領域の破損検出だけに使用する。鍵を利用可能と判断するのはwrapped-keyのintegrity検証、
または既知値による鍵検証operationである。このrecordはtarget localであり、sealed package formatには
含めない。

### 3.3 `K_model`のライフサイクル

1. `mtfs-seal`は、入力modelが同一でもpackage生成のたびに新しい32-byte keyを生成する。
2. 全payload chunkを`K_model`で暗号化し、`K_fleet`を使った`K_model`のAES-256-GCM envelopeを
   1個生成する。
3. Host processはclose後または任意のerror発生後に、`K_model`と一時plaintext bufferをzeroizeする。
4. targetではproviderがopen済み`K_fleet` handleを使ってenvelopeを認証・復号し、opaqueな
   `MODEL_PAYLOAD` handleを返す。raw keyを`sealed_blob`へ返してはならない。
5. hardwareがenvelope出力をkey slotへ直接連結できない場合、provider private context内に32-byteの
   plaintextが短時間だけ現れる可能性がある。その場合は直ちにimport/wrapし、bufferをzeroizeする。
   runtime RAM観測はv1 threat modelの対象外だが、この露出範囲はproviderごとに測定・記録する。
6. `mtfs_model_close()`後または任意の失敗後にhandleを破棄する。

### 3.4 証跡鍵のライフサイクル

providerはhardware RNGを使ってdevice key pairを生成する。相互運用可能な初期algorithm候補は
ECDSA P-256 with SHA-256とするが、最終選択はevidence実装spikeで決定する。RA RSIP-E50Dは
wrapped ECC keyを生成・利用できる。STM32で秘密鍵をdevice-boundに保つにはPKAとSAES/CCBの
wrapping flowが必要であり、現行targetではまだ検証していない。

Hostは`(device_id, algorithm, public_key, key_version)`を登録する。recordに格納するのはpublic keyの
識別子であり、private keyではない。model keyの交換時にevidence keyを交換したり、model keyから
導出したりしてはならない。

### 3.5 交換と将来のローテーション

v1は1以外の`key_id`または`key_version`を拒否する。将来versionでは`STAGED`と`ACTIVE`の2つの
device slotを追加し、test envelopeでcandidateを認証し、active metadataをatomicに切り替えた後で
旧slotをretireできる。複数envelope、revocation、anti-rollbackをpackageへ追加する場合は、
format minor/major versionを改めて判断する。v1へ暗黙には追加しない。

実鍵やprovisioning artifactはGitへcommitしない。将来固定test keyを追加する場合はHost test専用pathに
置き、`TEST ONLY - NOT FOR PRODUCTION`と明記し、production build設定では拒否する。

## 4. Sealed model package v1

### 4.1 エンコーディング規則

- integer fieldはunsigned little-endianとする。
- fixed preambleは160 byteとする。format 1.0のreaderは他の`preamble_size`を拒否する。
- 全reserved byteはwriterが0を書き、readerも0であることを検証する。
- file layoutは`preamble | metadata | key envelope | chunk 0 | ...`の厳密な逐次配置とする。
- `manifest`は正確な`preamble || metadata` byte列を意味する。
- v1のmetadata長上限は4096 byteとする。
- 全ての加算、乗算、offset、destination sizeはI/O前にchecked 64-bit arithmeticで検証する。
- trailing byte、sectionの重複または暗黙配置、非empty payloadのzero chunk、長さの不整合は
  format errorとする。

### 4.2 固定プリアンブル

| offset | size | field | v1の値／意味 |
|---:|---:|---|---|
| 0 | 8 | `magic` | ASCII `MTFSMOD`の後に`0x00` |
| 8 | 2 | `format_major` | 1 |
| 10 | 2 | `format_minor` | 0 |
| 12 | 4 | `preamble_size` | 160 |
| 16 | 4 | `manifest_size` | `160 + metadata_length` |
| 20 | 4 | `object_type` | 1 = AI model |
| 24 | 4 | `flags` | bit 0 = chunked。その他のbitは全て0 |
| 28 | 2 | `payload_aead` | 1 = AES-256-GCM-16 |
| 30 | 2 | `envelope_aead` | 1 = AES-256-GCM-16 |
| 32 | 4 | `key_id` | v1では1 |
| 36 | 4 | `key_version` | v1では1 |
| 40 | 16 | `package_id` | Host CSPRNGで生成する識別子。nonceの代用ではない |
| 56 | 16 | `model_id` | callerが割り当てるUUID byte列。Host toolは省略時にUUID v4を生成してよい |
| 72 | 8 | `model_version` | applicationから参照するversion。anti-rollbackの意味は持たない |
| 80 | 4 | `target_id` | 個別boardではなく互換性を共有するtarget/runtime family enum |
| 84 | 4 | `accelerator_id` | CPU/NPU provider enum |
| 88 | 4 | `model_format` | runtime model format enum |
| 92 | 4 | `reserved0` | 0 |
| 96 | 8 | `required_ram` | packagerが宣言する連続destinationの最小byte数 |
| 104 | 8 | `payload_plain_length` | 復号後modelの正確なbyte数 |
| 112 | 4 | `chunk_plain_size` | 4096..65536の2の冪 |
| 116 | 4 | `chunk_count` | payload長をchunk sizeでceiling divisionした正確な値 |
| 120 | 4 | `metadata_length` | 0..4096 |
| 124 | 4 | `key_envelope_length` | 48 (`32 ciphertext + 16 tag`) |
| 128 | 12 | `key_nonce` | `K_fleet`で使うrandom 96-bit GCM nonce |
| 140 | 8 | `payload_nonce_prefix` | この`K_model`で使うrandom 64-bit prefix |
| 148 | 12 | `reserved1` | 0 |

`required_ram`はpolicy判断用の宣言値であり、`payload_plain_length`は正確なload sizeである。
runtimeが隣接する追加workspaceを明示的に必要とする場合だけ、v1は
`required_ram >= payload_plain_length`を許可する。それ以外では両者を同値とする。
`mtfs_model_load()`は実際のdestinationを、宣言値とtarget policyの両方に対して検証する。

### 4.3 メタデータTLV

各entryは`type:u16 | flags:u16 | length:u32 | value:length`とし、8-byte境界まで0でpaddingする。
entryはtype昇順に並べ、typeの重複を拒否する。critical flag付きの未知entryは拒否し、noncriticalの
未知entryは無視してよい。初期typeはruntime ABI version、tensor arena requirement、model inputの
説明、長さを制限したUTF-8 display nameとする。path、secret、実行可能callbackは禁止する。
security上重要な固定fieldはTLVではなくpreambleへ置く。

Phase 4.1Aのcodecで曖昧さを残さないため、v1ではflag bit 0を`CRITICAL`とし、その他のflag bitを
拒否する。初期typeとcanonical valueは次のとおりとする。整数valueもunsigned little-endianである。

| type | value | v1の制約 |
|---:|---|---|
| 1 | runtime ABI version (`u32`) | lengthは4 byte |
| 2 | tensor arena requirement (`u64`) | lengthは8 byte |
| 3 | model input description (UTF-8) | 1..1024 byte、NUL禁止、正規UTF-8 |
| 4 | display name (UTF-8) | 1..255 byte、NUL禁止、正規UTF-8 |

未知noncritical typeはcanonical ordering/paddingを検査した上で無視する。既知typeのlengthやUTF-8が
不正な場合、未知flag、重複、降順、非zero paddingはformat errorとする。この明確化はpreamble、
section配置、AAD、暗号byte列を変更しない。

### 4.4 AEADの構成

key envelopeは次のとおりとする。

```text
ciphertext_32 || tag_16 = AES-256-GCM(
    key = K_fleet,
    nonce = key_nonce,
    aad = "MTFS-KEY-v1\0" || manifest,
    plaintext = K_model)
```

payload chunk `i`は`ciphertext(chunk_plain_length_i) || tag_16`として保存する。

```text
nonce_i = payload_nonce_prefix || LE32(i)
aad_i = "MTFS-CHUNK-v1\0" || manifest || LE32(i) || LE32(chunk_plain_length_i)
```

`K_model`はpackageごとに固有なので、prefix/counter namespaceも1回しか使わない。Host生成時には、
誤ってkeyを再利用した場合の影響も抑えるためrandom prefixを使う。writerは
`chunk_count > 2^32-1`、counter wrap、nonce生成失敗、RNG失敗を示すall-zero値を拒否する。
envelope nonceは独立して生成する。envelope nonceは同じ`K_fleet`を共有するため、Host policyでは
fleet key 1個あたりのpackage生成を最大2^32回に制限する。出力catalogを利用できる場合は、生成した
package IDとnonceを記録し、重複検出をfatal errorとする。GCM tagは16 byte必須とし、vendor APIが
対応していても短いtagは受理しない。

正確なmanifestを認証することで、object type、model identity/version、target/NPU/format、RAM size、
key selector、metadataをmodel keyと全chunkの両方へbindする。parserはAAD用にfieldを再serializeせず、
fileから境界検査付きで読み出したbyte列をそのまま使用し、別encodingの成立を防ぐ。

### 4.5 ロード状態機械とクリーンアップ

1. `open`: 160-byte preambleを読み、境界を検査し、長さを制限したmetadataを読む。期待file sizeを
   正確に計算して不一致やtrailing dataを拒否し、object typeを検証する。
2. `(key_id,key_version)`で選択したlocal `K_fleet` handleをopenする。
3. manifestをAADとして48-byte envelopeを認証し、opaqueな`K_model` handleを生成する。
   成功後に限り、`get_info`は認証済みmetadataを返してよい。
4. payload I/Oを開始する前に、認証済みmanifestから`payload_plain_length`を取得し、
   `payload_plain_length <= destination_size`および`destination_size >= required_ram`を検査する。
   この検査に失敗した場合はdestinationを変更せずに返す。検査成功後のzeroize範囲は、destination先頭から
   `payload_plain_length` byteと確定する。
5. chunkごとにciphertextをI/O bufferへ読み、chunk scratch bufferへ復号してtag verifyを呼出す。
   verify成功まではscratchのbyteを最終destinationへcopyしない。
6. verify成功後、正確なchunk plaintextを連続した最終destinationへcopyし、scratchをzeroizeする。
7. payload I/O開始後にerrorが発生した場合は、destination先頭から`payload_plain_length` byte、scratch、
   provider operation、import済み`K_model`をzeroizeし、authentication/format/I/O errorを返す。
   認証済みprefixも保持しない。`destination_size`の余剰部分やcaller所有の隣接workspaceは変更しない。
8. 全処理の成功時だけhandle stateを`LOADED`にする。applicationがmodelをNPUへ渡してよいのは
   その後だけである。

compilerによってstoreを除去されないproject共通primitiveでzeroizationを行う。単純な`memset`は
この契約を満たさない。DMAまたはNPUがbufferを参照する場合のcache clean/invalidateは、
target/providerの責務とする。

## 5. 復号方式の比較

| 評価項目 | model全体の一括read／復号 | whole-object AEADのstreaming出力 | 独立AEAD chunk（採用） |
|---|---|---|---|
| 最終model以外の追加RAM | in-placeを実証しない限りmodel sizeまでのciphertext copy | 小さいI/O/provider buffer | 1 chunkのscratch + I/O/alignment buffer |
| hardware作業buffer | 最大になる可能性がある | 小さい。vendor block alignmentが必要 | 小さい。chunkごとに独立operationを実行 |
| 未認証plaintext | final tag確認まで出力全体 | final tag確認まで最終model領域内で増加 | 最大1 scratch chunk |
| 認証失敗時のzeroization | model領域（destination先頭の`payload_plain_length` byte）と一時copy | model領域（destination先頭の`payload_plain_length` byte） | scratchとmodel領域（destination先頭の`payload_plain_length` byte） |
| 破損検出時点 | 最後 | 最後 | 破損したchunkの終端 |
| format overhead | 16-byte tag 1個 | 16-byte tag 1個 | chunkごとに16 byte。64 KiB時0.0244% |
| nonce管理 | packageごとに1個 | packageごとに1個 | 64-bit prefix + 検査済み32-bit index |
| header/AADとのbinding | 直接 | 直接 | 同じmanifestをchunkごとに使用 |
| random access | 不可 | 不可 | 将来のAPI/policy追加後に可能 |
| RA SD SPI | 大きなread/copyは高cost。現行4 KiB baselineは約274 KiB/s | sequential I/Oが可能 | 64 KiB chunk内で4 KiBずつ逐次read。早期失敗が可能 |
| STM32 SDMMC | 大きな連続transferが可能 | IDMA sequential readに適する | chunk内のmulti-block I/Oを維持。512-byte chunkは避ける |
| RSIP-E50D | one-shot可能 | GCM multi-shot Update/Verifyは文書化済み | 独立GCM operationに対応。import flowはspikeが必要 |
| STM32 SAES | GCM対応 | 正確なHAL streaming動作はspikeが必要 | chunkごとのone-shot fallbackが可能。DHUK flowはspikeが必要 |
| Host provider | 容易 | 容易 | 容易で、negative testも決定的に再現可能 |
| 連続NPU model領域 | payload全体が必要 | payload全体が必要 | payload全体が引き続き必要。chunkingでは削減されない |

whole-object streamingは採用しない。vendorのupdate APIは最終認証判定前にplaintextを出力する場合が
あり、その出力をNPU destinationへ直接書くと、未認証plaintextの公開範囲と失敗時cleanup範囲が
model全体に比例するためである。model全体の一括readも、大きなmodelを複製する可能性がある。

初期既定値は64 KiBとする。tag overheadが無視でき、FatFs/SD transaction overheadを償却でき、
scratchがEK-RA8P1の1664 KiB user SRAMおよびSTM32N657の現行1536 KiB Appli RAM regionに対して
小さいためである。実際のNPU/tensor arena配置は余裕が小さくなる可能性があるため、targetごとの
build-time上限を固定する前にPhase 4.1で4/16/64 KiBを測定する。package readerは宣言sizeを使用するが、
local policyにより拒否してよい。

## 6. 暗号プロバイダーインターフェース案

public model APIから見えるのは、このgeneric provider instanceだけである。正確なC宣言はPhase 4.1で
実装するが、ABIの形とsemanticsはここで確定する。

```c
typedef uint32_t mtfs_crypto_key_handle_t; /* 0は無効 */

typedef struct mtfs_crypto_provider {
    const struct mtfs_crypto_provider_api *api;
    void *context;
} mtfs_crypto_provider_t;

typedef struct mtfs_crypto_provider_api {
    mtfs_error_t (*open_persistent_key)(
        void *ctx, uint32_t key_id, uint32_t key_version,
        uint32_t required_usage, mtfs_crypto_key_handle_t *key);
    mtfs_error_t (*unwrap_key_aead)(
        void *ctx, mtfs_crypto_key_handle_t wrapping_key,
        const uint8_t nonce[12], const uint8_t *aad, size_t aad_size,
        const uint8_t *ciphertext, size_t ciphertext_size,
        const uint8_t tag[16], uint32_t output_usage,
        mtfs_crypto_key_handle_t *unwrapped_key);
    mtfs_error_t (*aead_decrypt_start)(
        void *ctx, mtfs_crypto_key_handle_t key, const uint8_t nonce[12]);
    mtfs_error_t (*aead_aad_update)(
        void *ctx, const uint8_t *data, size_t size);
    mtfs_error_t (*aead_decrypt_update)(
        void *ctx, const uint8_t *input, uint8_t *unauthenticated_output,
        size_t size, size_t *output_size);
    mtfs_error_t (*aead_verify_finish)(
        void *ctx, const uint8_t tag[16], uint8_t *tail,
        size_t tail_capacity, size_t *tail_size);
    void (*aead_abort)(void *ctx);
    void (*close_key)(void *ctx, mtfs_crypto_key_handle_t key);
} mtfs_crypto_provider_api_t;
```

provider契約:

- `unwrap_key_aead`はcallerに対してatomicに動作する。認証失敗時はhandleもraw keyも返さない。
- `aead_decrypt_update`の出力は、名前でも未認証であることを明示する。callerは
  `aead_verify_finish`成功まで出力をscratch内に保持する。
- v1ではprovider instanceごとにactiveなAEAD operationを1個だけ扱えればよい。競合時は
  `NOT_READY`を返す。serializationは明示的に行い、暗黙のglobal lockを仮定しない。
- vendorの32-bit lengthへnarrowingする前に全sizeを境界検査する。将来APIでproviderがin-place
  capabilityをadvertiseし、testするまではinput/output aliasを禁止する。
- `aead_abort`はidempotentとし、I/O、認証、timeout、cancel、media removal後にperipheral stateをclearする。
- key handleにはgeneration tagを持たせ、close/reset後のstale handleを失敗させる。
- tag不一致はsecurity固有の新error `MTFS_ERROR_AUTHENTICATION`へmapする。malformed formatと
  destination不足にも、実装Phaseで別々のerrorを割り当てる。
- providerが通常logへ出力するのはoperation/result識別子だけとし、key、復号plaintext、model内容は
  出力しない。nonce、tag、ciphertextはsecretではないが、byte dumpはtest artifactまたは明示的な
  診断機能に限定する。

RAでは`rsip_wrapped_key_t`を使用し、AEAD sequenceを
`R_RSIP_AES_AEAD_Init/AADUpdate/Update/Verify`へmapする。FSPではmulti-shot Updateと16-byte block
単位の出力動作が文書化されている。STM32ではDHUK-wrapped key importを使うSAES/CRYP HALへmapする。
現行repositoryにはN6 CRYP driverがまだ含まれないため、正確なsource/project追加は意図的に
実装Phaseへ延期する。Hostでは検証済みcrypto libraryを使い、独自AES実装は行わない。
wrapped handleはemulateするが、hardware保護があるとは主張しない。

将来のNSC serviceでは全Non-Secure pointerとlengthを検証し、AAD/control dataをSecure memoryへcopyし、
persistent keyとoperation stateをSecure側に保持して、同じresult semanticsを公開する。この将来置換で
package byte列やmodel APIは変更しない。

## 7. Model store API案

v1 APIではmodel全体をcaller所有の連続memoryへloadする。contextはcallerが確保し、heapは使わない。
concrete context sizeはprovider spike後に確定する。

```c
typedef struct mtfs_model_handle mtfs_model_handle_t;

typedef struct mtfs_model_open_config {
    mtfs_crypto_provider_t *crypto;
    uint32_t expected_target_id;
    uint32_t expected_accelerator_id;
    uint32_t max_chunk_size;
} mtfs_model_open_config_t;

typedef struct mtfs_model_info {
    uint16_t api_version;
    uint16_t struct_size;
    uint8_t model_id[16];
    uint64_t model_version;
    uint32_t target_id;
    uint32_t accelerator_id;
    uint32_t model_format;
    uint64_t model_size;
    uint64_t required_ram;
    uint32_t chunk_size;
} mtfs_model_info_t;

mtfs_error_t mtfs_model_open(
    mtfs_model_handle_t *handle, const mtfs_model_open_config_t *config,
    const char *path);
mtfs_error_t mtfs_model_get_info(
    const mtfs_model_handle_t *handle, mtfs_model_info_t *info);
mtfs_error_t mtfs_model_load(
    mtfs_model_handle_t *handle, void *destination,
    size_t destination_size, size_t *loaded_size);
mtfs_error_t mtfs_model_close(mtfs_model_handle_t *handle);
```

`open`はformatを検証し、`K_model` envelopeを使ってmanifestを認証する。object type不一致、未対応の
target/NPU/format、未対応key selector、上限超過chunkはreturn前に拒否する。このため、成功handleに
対する`get_info`が未認証metadataを返すことはない。v1の`load`は同期APIであり、
`destination_size >= required_ram`を要求し、正確な`model_size`（`payload_plain_length`）を
`loaded_size`へ返す。payload I/Oの開始前に、認証済みmanifestからzeroize可能な範囲を確定する。
`destination_size`が`required_ram`または`payload_plain_length`より小さい場合は、destinationを変更せず
`loaded_size=0`として失敗する。この検査後に失敗した場合は、destination先頭から
`payload_plain_length` byteをzeroizeし、`destination_size`の余剰部分およびcaller所有の隣接workspaceは
変更しない。`close`は初期化途中でも安全に呼出せ、FatFs objectと全provider handleをcloseする。

このAPIはmodelをNPUへsubmitしない。runtime固有のcache maintenanceとNPU呼出しは
application/provider層に残す。将来のpartial/direct loadには新APIが必要であり、v1 whole-load規則を
弱める形では追加しない。

genericなparsing/AEAD/cleanupは`sealed_blob`が所有する。`model_store`はobject typeとmodel metadataの
policyを提供し、crypto処理やfile arithmeticを重複実装しない。

## 8. 4つの拡張機能の責務

### `sealed_blob`と`model_store`

- `sealed_blob`: canonical parser、checked size/offset arithmetic、manifest AAD、key envelope、chunk AEAD、
  provider呼出し、state machine、error cleanup、zeroizationを担当する。
- `model_store`: AI object type、model ID/version、target/NPU/format allow-list、RAM宣言、public load APIを
  担当する。
- 依存方向は`model_store -> sealed_blob -> crypto_provider + bounded file reader`とする。

### `secure_media_sink`

- middlewareがtemporary-file transactionを所有する。固有temp fileを作成し、inputをtransform
  callback/providerへ渡し、変換済みoutputだけを書き、sync、close後、FatFs semanticsが許す範囲で
  atomicにrename/commitする。
- 顔検出／マスキングalgorithmとNPU呼出しはproviderまたはsample applicationの責務とする。
- transform失敗、media removal、sync失敗時はtempを削除または無効化し、可能な限り直前にcommit済みの
  objectを維持する。recovery journal semanticsは将来設計とする。
- block filterではなくfile-level commit middlewareとする。`model_store`経由で顔modelをloadしてもよいが、
  dependency cycleを避けるため、このorchestrationはapplicationが所有する。

### `storage_sentinel`

- cached `mtfs_block_diagnostics_t`、`mtfs_media_diagnostics_t`、target typed snapshot 1個を読む。
- callerがmonotonic timestampを与える。feature抽出では`reset_epoch`または`media_generation`が変化した
  snapshot pairを拒否し、validity maskとsaturating counterを正しく扱う。
- state、anomaly score、reason bitを返す。media I/Oは行わず、file/model内容も所有しない。
- 保存、throttling、remount、capture停止はapplication policyであり、sentinel自身では行わない。

### `evidence_log`

- canonical recordにはsequence、caller timestamp、event type、file hash、previous-record hash、選択した
  diagnostics summary、device public-key ID、signatureを含める。
- deviceは各recordをhashし、`K_evidence_priv`で署名する。Hostはsignatureとhash linkageをoffline検証する。
- trustedな開始点またはheadに対するrecordの欠落、並べ替え、編集を検出できる。chain全体のrollbackや
  truncationを検出するには、Hostが以前受理したheadを保持するか、将来のmonotonic anchorが必要である。
- diagnostics summaryはapplicationから渡す。`evidence_log`は`storage_sentinel`へ依存しない。

semantic tag/sidecar、adaptive retention、event recorderは本設計の対象外とする。

## 9. ホストツール

### `mtfs-keygen`

- OS CSPRNGから正確に256 bitを取得し、利用できなければfail closedする。
- Host OSが対応する場合は制限的なpermissionを設定する。
- 鍵を表示しない。明示的なbinary出力またはprotected container出力に対応する。
- 非secretのkey ID/version metadataは別に出力する。

### `mtfs-seal`

- raw modelと検証済みmodel metadataを読む。
- 新しい`K_model`、`package_id`、`key_nonce`、payload nonce prefixを生成する。
- temporary outputへ書き、全chunk tagを計算し、flushしてから成功時にrenameする。
- final layoutを自己検証し、必要に応じてcommit前にthrowaway bufferへ復号してhashを比較する。
- 失敗時はkeyをzeroizeし、不完全なoutputを削除する。

### Provisioning支援

- RA adapterはRenesas SKMT/RFP secure injectionが要求するinputを準備するか、開発用であることを
  明記したUART injection flowを提供する。irreversible programmingは自動実行しない。
- STM32 adapterは認証済みdevelopment commandを送るか、device固有のwrapping requestを構築する。
  targetなしでHostがDHUK-wrapped blobを生成できるとは仮定しない。
- provisioning transcriptにはdevice ID、public metadata、statusだけを記録し、raw key byteは含めない。

## 10. 対応可否マトリクス

| capability | Host | EK-RA8P1 / RSIP-E50D | STM32N657 / SAES | 状態 |
|---|---|---|---|---|
| AES-256-GCM、16-byte tag | standard library | FSP protected modeはGCM 128/192/256対応 | SAESはGCM 128/256対応 | 文書確認済み |
| multi-shot AEAD | library依存、必須 | FSP Init/AADUpdate/Update/Verifyは文書化済み | HAL sequenceの検証が必要 | RA可、ST spike待ち |
| applicationからのHUK/DHUK読出し | 該当なし | 不可。hardware wrapping rootとして使用 | 不可。SAES内部のderived key | 設計上禁止 |
| device-bound `K_fleet` blob | test時だけemulate | RSIP wrapped key、256-bit HUK | DHUKを使うSAES wrapped-key mode | 文書確認済み、統合spike待ち |
| envelopeからopaque `K_model`への変換 | software handle | 復号直後のInitialKeyWrap/importが候補 | 復号直後のSAES wrap/importが候補 | 両targetでspike待ち |
| tag失敗時cleanup | 決定的test | Verify error + middleware scratch zeroization | HAL error + middleware scratch zeroization | target spike待ち |
| device evidence署名 | software test key | wrapped ECC/Ed25519 capability | PKA + SAES/CCBが候補 | 後続evidence spike |
| TrustZone NSC隔離 | 該当なし | 現行flat buildには存在しない | splitなし。現行imageはFullSecure | 将来のみ |
| 現行storage I/O | file-backed block device | SD over SPI、4 KiB baselineで約274 KiB/s | SDMMC IDMA、4 KiB baselineで約4.0 MiB/s | 実機検証済み |
| 連続model RAM | Host allocation | NPU/runtime依存。1664 KiB user SRAM + external memory | NPU/runtime依存。現行Appli linkerは1536 KiB RAM | 配置spike待ち |

このmatrixではperipheral capabilityと統合完了を区別する。文書化されたalgorithmでも、現行projectで
正確なwrapped-key/tag-failure flowをbuild・実行するまでは実装済みとしない。

## 11. Phase 4.1以降の実装順序

### Phase 4.1: フォーマットとプロバイダーのスパイク

1. Host専用format parser/writerとknown-answer/negative testを追加する。全fieldのtruncation、integer overflow、
   duplicate/critical TLV、trailing data、nonce/index境界、header/metadata/cipher/tag mutation、誤った
   fleet key、destination zeroizationをtestする。
2. TrustZone modeを変更せず、破棄可能なtarget branch/configでRA providerをspikeする。AES-256 wrapped keyの
   永続化、32-byte envelope import、4/16/64 KiB GCM、partial final block、tag失敗、abort、rebootを通す。
3. 現行FullSecure LRUN AppliでSTM32 providerをspikeする。実装Phaseでは必要なCube CRYP/SAES sourceだけを
   追加し、clock/RIF/security contextを設定する。同じtest vectorに加えてHDPL reset/rebootを通す。
4. stack/BSS/scratch、crypto throughput、SD+crypto pipelineを測定する。合否条件は、link map上の重複なし、
   log/map/repository内のkeyなし、verify前のscratch外plaintextなし、payload I/O開始後の全failure injectionで
   destination先頭の`payload_plain_length` byteがzeroizeされ、余剰部分が変更されないこととする。
5. 両spike成功後に限り、provider context size、target最大chunk policy、error enumを確定する。

### Phase 4.2: sealed blobとmodel store

generic parser/state machine、Host provider、public model API、RA/ST provider、target known-answer testを
実装する。同じfleetに属する両target providerで、1つのHost toolが生成したpackageをcross-testする。
Host parserのfuzzingと、target testと共有するmutation corpusを追加する。

### Phase 4.3: secure media sinkとstorage sentinel

model crypto pathから独立して、変換成功後だけcommitする仕組みを実装する。既存diagnosticsから
fixed-pointで決定的なfeatureを抽出し、policyはsample applicationに残す。

### Phase 4.4: evidence log

RA/STのdevice key生成、wrapping、署名を検証し、canonical record encodingを定義してHost verifierを
構築し、trusted-headの保持方法を規定する。model keyの導出・再利用は行わない。

### 後続セキュリティフェーズ

最初にpublisher署名とanti-rollbackを検討する。TrustZone NSC crypto serviceは、untrusted Non-Secure
firmwareを含むthreat model、version付きpointer-validation ABI、dual-image update/recovery、hardware testを
揃えた場合だけ採用する。v1 packageの前提条件にはしない。

## 12. 公式資料

### Renesas

- [FSP: Renesas Secure IP protected mode](https://renesas.github.io/fsp/group___r_s_i_p___p_r_o_t_e_c_t_e_d.html)
- [FSP: Secure Key Injection](https://renesas.github.io/fsp/group___r_s_i_p___k_e_y___i_n_j_e_c_t_i_o_n.html)
- [RA Family: Secure User Keyの注入と更新](https://www.renesas.com/en/document/apn/injecting-and-updating-secure-user-keys)
- [RA8P1 product page](https://www.renesas.com/en/products/ra8p1)
- [RA8P1 memory architecture application note](https://www.renesas.com/en/document/apn/getting-started-ra8p1-memory-architecture-configurations-and-topologies)
- [RA8 MCUでのEthos-U NPU利用](https://www.renesas.com/en/document/apn/using-ethos-u-npu-ra8-mcus)

FSP資料で、RSIP-E50D protected modeのAES-GCMとmulti-shot AEAD APIを確認した。key-injection APIは
provider wrapped keyを出力し、AES-256に対応する。Renesas資料には256-bit HUK wrapping rootが記載され、
copyしたwrapped keyを別MCUでは利用できないことが示されている。production provisioningでは
開発用plaintext経路ではなく、UFPK/W-UFPK flowに従う。

### STMicroelectronics

- [RM0486: STM32N647/657 reference manual](https://www.st.com/resource/en/reference_manual/dm00769900.pdf)
- [STM32N6x5/x7 datasheet](https://www.st.com/resource/en/datasheet/dm01125716.pdf)
- [UM3451: STM32N6 security guidance](https://www.st.com/resource/en/user_manual/um3451-stm32n6xx-security-guidance-for-sesip-level-3-certification-stmicroelectronics.pdf)
- [STM32N6 security機能](https://wiki.st.com/stm32mcu/wiki/Security%3ASecurity_features_on_STM32N6_MCUs)
- [sensitive keyの保護](https://wiki.st.com/stm32mcu/wiki/Security%3ASensitive_key_protection)
- [公式STM32N6 HAL driver repository](https://github.com/STMicroelectronics/stm32n6xx-hal-driver)

datasheetでSAES AES-128/256 GCM/CCMとDHUK/BHK hardware-key loadingを確認した。ST資料には、DHUKを
softwareから読み出せないこと、wrapped-keyのunwrap結果がwrite-only SAES key registerへloadされることが
記載されている。UM3451にはDHUKを使った認証付きkey storageも記載されている。Cube FW_N6 V1.3.0への
正確なHAL統合は推測せず、Phase 4.1の検証結果として確定する。

### 標準資料

- [NIST SP 800-38D: GCM and GMAC](https://csrc.nist.gov/pubs/sp/800/38/d/final)
- [Arm TrustZone for Armv8-M training](https://developer.arm.com/Training/TrustZone%20for%20Armv8-M)

## 13. 参照したリポジトリ内の根拠

- `tests/targets/stm32n6570_dk/basic/mtfs_stm32n6570_dk_test_basic.ioc`には
  `Mcu.ContextProject=FullSecure`が記録されている。
- `tests/targets/stm32n6570_dk/basic/README.md`はAppliをsecure LRUN applicationと記載している。
- RA FSP生成物`bsp_mcu_family_cfg.h`は、`_RA_TZ_SECURE`と`_RA_TZ_NONSECURE`のどちらも未定義の場合、
  両TrustZone build flagを0にする。現行flat projectはどちらも定義していない。
- `src/ports/ra_fsp/trustzone/`と`src/ports/stm32_cube/trustzone/`はREADMEだけの予約領域である。
- `docs/diagnostics.md`と現行headerは、`storage_sentinel`へ渡すcached diagnostics inputを定義している。
- 実測済みI/O baselineと現行STM32 Appli RAM regionは、target README/linker scriptを根拠とする。

現行RA projectが記録するFSPは6.5.0だが、本書作成時点で公開FSP documentation linkはrelease 6.5.1を
指していた。RSIP-E50D key-injection application noteはFSP 6.4.0を対象としているが、Phase 4.1では
installed 6.5.0 packに対して記載APIをcompileし、その後にAPI signatureを確定扱いとする。
