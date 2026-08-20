# ADR 0003: Secure AI Storage の初版境界と sealed model 形式

- Status: Accepted for Phase 4.0; provider integration is hardware-validation pending
- Date: 2026-08-18
- Targets: EK-RA8P1 (RA FSP 6.5.0), STM32N6570-DK (STM32Cube FW_N6 V1.3.0), Host tests
- Scope: architecture and interface design only

## Context

microT-FSへ、SDカード上のAIモデル保護、保存前の顔マスキング、diagnosticsを入力とする
状態異常検出、署名付きevidence chainを追加する計画がある。Phase 4.0ではproduction codeや
target projectを変更せず、これらが同じ鍵や責務を誤って共有しない境界を確定する。

現行targetを再確認した結果は次のとおりである。

- STM32N6570-DKはCubeの`FullSecure` projectで、Appliはsecure LRUN applicationである。
  Secure/Non-Secureの2 image構成ではない。
- EK-RA8P1はflat buildである。生成済みFSP headerでは
  `BSP_TZ_SECURE_BUILD=0`、`BSP_TZ_NONSECURE_BUILD=0`となる。
- `src/ports/*/trustzone/`はREADMEだけの予約領域で、NSC crypto serviceは存在しない。

STM32の`FullSecure`はCPUがSecure stateでapplicationを実行する構成であり、同じimage内の
software component間を隔離するものではない。RAのflat buildにもSecurity stateによる分離は
ない。したがって初版を「TrustZoneで鍵を隔離した」と表現してはならない。一方、RA8P1の
RSIP-E50D protected modeとSTM32N657のSAESは、HUK/DHUKをsoftwareへ読み出さず、
device固有のwrapped keyを利用できる。

詳細な形式、API、比較、検証計画は
[`../security/secure-ai-storage.md`](../security/secure-ai-storage.md)に定める。

## Threat model

### 信頼するもの

- 正規firmware、正規application、正規Host tool
- Host toolを実行する環境と、利用者が管理するprovisioning経路
- vendor hardware cryptoと乱数源が仕様どおり動作すること

### 保護対象

- SDカードの盗難、およびSD内容だけの複製
- SD上のAIモデルの内容解析
- package header、metadata、暗号化モデル、暗号化された`K_model`の改ざん
- `K_fleet`を持たない者による別モデルへの差し替え
- evidence recordの編集、並べ替え、途中削除

同じsealed packageを同じfleetの複数boardで利用する要件と、byte-for-byteのSD cloneを
同じfleet内でも拒否する要件は両立しない。初版の「SD複製への保護」は、SDだけを複製しても
`K_fleet`をprovisioning済みでないdeviceでは利用できないことを意味する。同じfleetの正規device
への複製は意図した利用方法であり、検出しない。

### 初版の対象外

- chipの侵襲的解析、side-channel/fault injectionに対する製品認証
- 正規firmware実行中のRAM解析
- firmware/applicationの完全侵害
- 過去の正規modelへのrollback、packageの利用回数制限
- remote attestation、server認証、OTA update
- transparentなFatFs volume全体の暗号化
- Host verifierが既知の最終headを保持しない場合のevidence chain全体の置換・truncation

debug lock、OTP programming、lifecycle変更、tamper設定はPhase 4.0でも4.1でも自動的に行わず、
製品provisioningの別判断とする。

## Decision

### 1. 初版ではTrustZone splitを採用しない

現行のFullSecure/flat projectを維持する。TrustZone splitはproject生成、起動、memory map、
peripheral attribution、NSC ABI、両imageの更新とtestを同時に変えるため、model package形式と
hardware crypto導入の検証を不必要に結合する。今回のthreat modelは正規firmwareを信頼しており、
初版に必要なのはHUK/DHUK backed keyとAEADである。

将来のsplitでは、同じ`mtfs_crypto_provider`のtarget実装だけをNSC clientへ置き換える。
provider APIはraw keyを返さず、key handle、AEAD operation、sign operationだけを公開する。

### 2. 鍵用途を分離する

- `K_fleet`: 利用者または製品群ごとのAES-256 key。Hostで生成し、deviceごとにHUK/DHUKで
  wrapしたprovider-native blobとして保存する。初版のactive keyは`key_id=1`、
  `key_version=1`の1個だけとする。
- `K_model`: packageごとにHostのCSPRNGで新規生成するAES-256 key。model payloadを暗号化し、
  `K_fleet`によるAES-256-GCM envelopeをpackageへ格納する。
- `K_evidence_priv`: device内で生成する非対称署名秘密鍵。`K_fleet`/`K_model`と共有しない。
  device固有wrapped formで保存し、公開鍵だけをHost verifierへ登録する。

HUK/DHUKは読出し可能な鍵として扱わない。providerがhardware key selectionまたはwrapped key
importに使うrootであり、microT-FS APIへbyte列として現れない。

EK-RA8P1のcontest profileでは、ボードごとの初回処理をtrusted環境のUART/XMODEMと専用
provisioning firmwareで行う。RSIP-E50D Compatibility ModeのInitialKeyWrap APIでraw
`K_fleet`を52-byte HUK-wrapped keyへ変換し、board上OSPI末尾8 KiBのdual-slot recordへ保存する。
通常版はrecordをSRAMへloadしてPSAのvolatile wrapped-key handleへimportする。KUK、RFP/SKMT、
内部MRAM予約、remote update、anti-rollbackは採用しない。OSPI recordを失った場合はtrusted環境で
再provisioningする。plaintext inputを許すため、このprofileはproduction provisioningではない。
詳細は[`../security/ek-ra8p1-ospi-key-provisioning.md`](../security/ek-ra8p1-ospi-key-provisioning.md)に定める。

### 3. sealed modelはchunked AES-256-GCMとする

初版formatは64 KiBを既定chunk sizeとする独立AEAD chunk方式を採用する。formatは4 KiBから
64 KiBまでの2の冪を許可し、target policyは最大64 KiBとする。各chunkは16-byte tagを持つ。
96-bit nonceはpackage固有の64-bit random prefixと32-bit chunk indexから構成し、同じ
`K_model`で再利用しない。`chunk_count`は32-bit範囲内に制限する。

headerとmodel metadataは`K_model` envelopeおよび全payload chunkのAADへbindする。これにより、
metadataだけの差し替えも失敗する。NPUが連続領域を要求する場合、最終model RAMはpayload長と
同じだけ必要であり、chunkingはこれを削減しない。chunkingの目的は、未認証平文を最大1 chunk
のscratchへ閉じ込め、認証後だけ最終領域へcopyすること、早期の破損検出、将来の部分loadである。

### 4. model発行者署名は初版に含めない

初版が保証するのはconfidentiality、integrity、`K_fleet` holderによるpackage authenticityである。
`K_fleet`を持つ者は任意のpackageを作成できるため、model publisher固有のidentityは保証しない。
publisher署名、certificate、revocation、anti-rollback、OTA model updateは将来拡張とする。

### 5. file-level extensionとして配置する

既存の`src/extensions/security/`と`src/extensions/ai/`を使い、実装Phaseでは次を候補配置とする。

```text
src/extensions/security/sealed_blob/
src/extensions/security/secure_media_sink/
src/extensions/security/evidence_log/
src/extensions/ai/model_store/
src/extensions/ai/storage_sentinel/
src/ports/ra_fsp/crypto/
src/ports/stm32_cube/crypto/
src/ports/host/crypto/
```

`sealed_blob`はFatFs上のobject formatであり、transparent block encryptionではないため
`src/block/filters/`には置かない。`model_store`だけがmodel/NPU metadataを解釈する。

## Consequences

- SD盗難時にmodel本体と`K_model`は平文で得られない。device固有wrapped `K_fleet` blobを別deviceへ
  copyしても利用できない。
- 同一fleetの1台が完全侵害され`K_fleet`が利用可能になると、そのfleet向け全packageの生成・復号が
  可能になる。fleet範囲を小さくする運用が必要である。
- FullSecure/flat applicationの侵害からkey operationを隔離する保証はない。hardware engineが
  HUK/DHUKおよびstored wrapped keyを保護する範囲だけが追加保証である。
- `mtfs_model_load()`はpayload I/O開始前に、認証済みmanifestから`payload_plain_length`を取得して
  destinationへ収まることを検査し、失敗時に消去可能な範囲を確定する。サイズ検査に失敗した場合は
  destinationを変更しない。payload I/O開始後に後続chunkを含む処理が失敗した場合は、認証済みprefixを
  保持せず、destination先頭から`payload_plain_length` byteをzeroizeする。`destination_size`の余剰部分や
  caller所有の隣接workspaceは変更しない。applicationは`mtfs_model_load()`成功前のdestinationをNPUへ
  渡してはならない。
- 64 KiB scratchとI/O/alignment bufferが最終model領域に追加して必要になる。両targetの搭載SRAM
  から設計上は許容できるが、NPU runtimeとの同時配置はlink mapと実機で確認する。
- package formatはtarget非依存だが、`K_fleet`のdevice保存blobはprovider固有で互換ではない。

## Phase 4.1 acceptance gates

次のspikeが全て通るまでprovider実装をproduction-readyとしない。

1. RA8P1でAES-256 wrapped keyを再起動後にimportし、GCMのchunk単位one-shot decrypt、AAD、
   tag failureを確認する。tag failure時はscratch外に平文を出さない。multipartは必須条件にしない。
2. RA8P1でpackage envelopeから得た32-byte `K_model`をprovider-native wrapped keyへ直ちに
   importできること、raw bufferをzeroizeできることを確認する。
3. STM32N657 FullSecure AppliでSAESのDHUK wrapped-key mode、AES-256-GCM、tag failure、
   HDPL/security-context条件を確認する。Cube FW_N6 V1.3.0のCRYP/SAES driver追加範囲も確認する。
4. 両targetで4/16/64 KiB chunk、非16-byte最終chunk、misaligned FatFs buffer、in-place禁止時の
   bounce経路、同時SD I/Oとのcache/DMA整合を確認する。
5. 64 KiB scratch、provider context、NPU model領域、tensor arenaを含むlink mapが重ならず、
   `mtfs_model_load()`のpayload I/O開始後の失敗時にdestination先頭の`payload_plain_length` byteとscratchが
   zeroizeされ、`destination_size`の余剰部分が変更されないことを確認する。

合否条件の詳細は設計文書の「Phase 4.1以降」を参照する。
