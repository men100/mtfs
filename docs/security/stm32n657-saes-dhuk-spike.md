# STM32N657 SAES/DHUK hardware crypto provider spike

- Phase: 4.1B-ST
- Baseline: STM32Cube FW_N6 V1.3.0, STM32N6570-DK FullSecure LRUN
- Status: **HARDWARE PASS** on STM32N6570-DK
- Safety: key-store erase/program was limited to the reserved final two 4 KiB NOR subsectors; no OTP write, lifecycle change, tamper setup, or debug lock was performed

This document records both the Cube-source mapping and the completed STM32N6570-DK hardware run. Build success alone was not treated as a hardware result; the status was changed to **HARDWARE PASS** only after provisioning, reset/power-cycle persistence, GCM interoperability/negative tests, and the sealed-package path passed on the board.

## SAES/DHUK mapping

The provider is implemented in `src/ports/stm32_cube/crypto/mtfs_stm32_saes.c`. It uses only the STM32N6 HAL CRYP/CRYPEx interface; it does not contain a software AES or GCM implementation.

| Operation | Cube FW_N6 V1.3.0 path |
|---|---|
| Wrap a 32-byte AES-256 application key | `HAL_CRYPEx_WrapKey()` with `KeySelect=CRYP_KEYSEL_HW` (DHUK), `KeyMode=CRYP_KEYMODE_WRAPPED`, AES-256, and key protection disabled |
| Use the wrapped key without returning plaintext to software | `HAL_CRYPEx_UnwrapKey()` in ECB wrapped-key mode loads the key into write-only SAES state; GCM then runs with `KeyMode=CRYP_KEYMODE_NORMAL` without exporting the key to software |
| GCM tag | `HAL_CRYPEx_AESGCM_GenerateAuthTAG()` after the payload operation |
| Clear transient SAES state | `HAL_CRYP_DeInit()`, SAES peripheral force/release reset, context/buffer zeroization |

The Cube API represents an AES-256 wrapped key as eight 32-bit words: exactly 32 bytes, with 4-byte alignment. Wrap and unwrap use the official example's 32-bit no-swap setting; the provider packs the raw AES byte string into words and switches to 8-bit data swapping only after unwrap for GCM byte-string input. The repository record provider ID is `MTFS_WRAPPED_KEY_PROVIDER_STM32_SAES_DHUK`; it is deliberately different from the 52-byte RA RSIP-E50D blob.

For a 96-bit GCM nonce, the provider loads the three nonce words and the initial counter word `2`, as required by the STM32 HAL contract. AAD and non-block-multiple final payloads are passed through one logical AEAD operation. The provider limit is 64 KiB payload and 4 KiB AAD; because the HAL payload-length argument is 16 bit, exactly 64 KiB is submitted internally as 65,520 bytes plus 16 bytes with `CRYP_KEYIVCONFIG_ONCE`. This is not a 65,535-byte cryptographic limit. Static aligned buffers make the spike single-threaded and non-reentrant; DMA and shared-key optimizations are not part of this phase.

### Authentication failure behavior

`HAL_CRYP_Decrypt()` does not accept an expected GCM tag and can return `HAL_OK` after writing candidate plaintext. `HAL_CRYPEx_AESGCM_GenerateAuthTAG()` returns the computed tag; the HAL does not compare it with the package tag. Therefore a HAL status alone is never treated as authentication success or failure.

The middleware always decrypts into private scratch, generates the tag, compares all 16 bytes in constant time, and copies plaintext to the caller only after a match. A mismatch is reported as `MTFS_STM32_SAES_AUTH_FAILED`; scratch and the caller-visible output are zeroized. The actual HAL status and error code remain diagnostic data, but no broad HAL error is reclassified as an authentication rejection.

## Clock and security context

The provider enables and initializes SAES and RNG clocks and resets SAES after each operation. The current target remains the existing `FullSecure` LRUN application; it does not introduce a Secure/Non-Secure split. `crypto-info` reports the static FullSecure build identity, CPU privilege (`CONTROL`), selected SAES registers, and public NOR geometry.

The hardware run reported `build=FullSecure`, `privileged=1`, and `CONTROL=0x00000000`. Successful SAES wrap/unwrap/GCM, RNG-backed provider initialization, XSPI2 NOR provisioning/readback, and SDMMC package access establish that the effective attribution permits the tested secure privileged image to access those resources. The same DHUK-wrapped record remained usable after normal restart and complete power removal/reapply, establishing a compatible derivation context for this board and configuration. No per-peripheral raw RIF/CID register dump or H5-style HDPL claim is inferred; STM32N6 product lifecycle, tamper, and HKLOCK hardening remain separate production decisions.

The SESIP guidance also describes tamper, HKLOCK, redundant checks, and fault/side-channel hardening. Those production/certification measures are outside this contest spike and must not be inferred from the provider build.

## External NOR reservation

The board device is the Macronix MX66UW1G45G: 1 Gbit (128 MiB), 4 KiB subsector erase, and 256-byte page program. It is memory-mapped from `0x70000000`. The repository FSBL configuration places the application image at NOR offset `0x00100000` with a fixed `0x00080000` reservation, so that region ends at `0x0017ffff`.

The key store reserves the next two complete subsectors:

| Purpose | NOR offset | CPU memory-mapped address | Size |
|---|---:|---:|---:|
| slot A | `0x07FFE000` | `0x77FFE000` | 4 KiB |
| slot B | `0x07FFF000` | `0x77FFF000` | 4 KiB |

This placement reserves the final 8 KiB of the 128 MiB MX66UW1G45G. It must remain excluded from every firmware image and explicit firmware erase range. Normal STM32CubeProgrammer image programming erases only the sectors needed by the image and does not touch these slots; an explicitly requested whole-chip erase does erase both slots and requires trusted reprovisioning.

The N6570-DK BSP binding initializes XSPI2 and the NOR in OPI DTR mode and checks the detected 128 MiB/4 KiB/256-byte geometry before exposing read/erase/program callbacks. The first NOR open acquires the HAL timebase before BSP initialization, so `crypto-info` returns to the console instead of stalling in a BSP delay. Initial provisioning erased slot A only and committed generation 1/key version 1 with readback verification; the normal application subsequently loaded the same record after restart and complete power removal.

## Dual-slot record and commit

Each 4 KiB slot begins with a 64-byte record containing magic, format/header size, generation, key ID, key version, wrapped length, CRC-32, commit word, and the 32-byte wrapped blob. The rest of the subsector is unused. The commit sequence is:

1. Scan both slots and select the valid record with the greatest generation.
2. Reject first provisioning if a valid record already exists unless the dedicated update command was explicitly selected.
3. Erase only the inactive 4 KiB slot.
4. Program a staged record whose commit word remains erased (`0xffffffff`).
5. Read back and compare every staged byte.
6. Program the 4-byte commit marker last.
7. Read, decode, CRC-check, and select the committed record.

Host tests use an 8 KiB NOR simulator and cover empty media, commit-last ordering, initial provisioning, default reprovision rejection, interruption before commit preserving the old slot, generation-2 update, corruption fallback, and generation exhaustion. Power cuts at every physical program boundary on the real NOR remain an optional extended test.

## Provisioning boundary

The dedicated firmware is under `apps/key-provision/targets/stm32n6570_dk/`; it is not present in the normal application. `provision-xmodem` receives one XMODEM-CRC block whose first 32 bytes are the key and whose remainder must be standard `0x00`/`0x1a` padding, then requires EOT. Bad CRC/block number, non-padding excess, additional/incomplete input, and timeout are rejected. Because classic XMODEM carries no exact file length, a sender must still be instructed to send a 32-byte file; an extra all-zero or all-`0x1a` suffix is indistinguishable from transport padding.

The raw key exists only in an aligned stack buffer during receipt and wrapping. It is zeroized immediately after `HAL_CRYPEx_WrapKey()`, before positive/negative wrapped-key validation and before any NOR write. Logs contain only public metadata and status. `provision-xmodem` rejects an existing valid record; `update-xmodem` is an explicit operator action that increments the key version and uses the inactive slot. The firmware contains no OTP, lifecycle, tamper, or debug-lock operation.

## Sealed package integration

The normal basic target provides:

- `crypto-info`: public configuration and runtime diagnostic registers; no key bytes.
- `crypto-consistency`: disposable RAM key wrap/reload and AES-256-GCM for empty, 37-byte, 4 KiB, 16 KiB, and 64 KiB payloads, AAD, partial final block, and provider reinitialization.
- `crypto-package-test`: mounts `0:`, reads only the fixed manifest, loads wrapped `K_fleet` from NOR, authenticates/decrypts the `K_model` envelope, immediately wraps and zeroizes raw `K_model`, then authenticates/decrypts 4096- and 904-byte chunks and checks the 5000-byte public pattern.
- `crypto-negative`: changes only RAM copies of the envelope tag, chunk ciphertext, and chunk tag; each must return `AUTH_FAILED` with zero caller output.

The package path is `0:/MTFSTEST.MTF`, and the test expects the same 5280-byte fleet-specific Phase 4.1A package used on RA. It does not consult `MTFSTEST.TXT` for a security decision and never loads the entire package into RAM. FatFs, file handles, scratch, raw model key, wrapped transient model key, and provider state are cleaned on success and failure.

Unlike RA, STM32 uses a 32-byte SAES/DHUK blob rather than a 52-byte RSIP blob and performs the GCM tag comparison in middleware because of the HAL contract. The sealed package bytes and key hierarchy do not change.

## Build and test result

As of 2026-08-21:

- Host CMake clean build and CTest: 2/2 PASS under WSL with GCC 15.2.0 and OpenSSL 3.5.5, including the simulated NOR tests.
- Dedicated provisioning Appli Debug clean build: 0 errors, 0 warnings; text 58,164 bytes, data 3,412 bytes, BSS 147,320 bytes.
- Dedicated provisioning FSBL Debug clean build: 0 errors, 0 warnings; text 61,036 bytes, data 12 bytes, BSS 3,660 bytes.
- Normal Appli Debug clean build: 0 errors, 0 warnings; text 205,520 bytes, data 3,412 bytes, BSS 358,948 bytes.
- STM32N6570-DK provisioning PASS: XMODEM received exactly 32 key bytes; NOR slot `0x07ffe000` committed generation 1/key version 1 and verified with one valid slot.
- `crypto-consistency` PASS: OpenSSL 37-byte ciphertext/tag interoperability, payload sizes 0/37/4096/16384/65536 bytes, and a 37-byte provider-reinitialization check.
- `crypto-negative` PASS: low-level tag/ciphertext mutation and package envelope-tag/chunk-ciphertext/chunk-tag mutation all failed closed; caller output and scratch were zeroized.
- `crypto-package-test` PASS for `0:/MTFSTEST.MTF`: envelope, immediate model-key rewrap/raw-key zeroization, 4096- and 904-byte chunks, 5000-byte known plaintext, and SD/FatFs cleanup.
- The NOR record and all normal-application crypto/package tests passed again after complete power removal/reapply. Existing SDMMC IDMA/IRQ, roundtrip, diagnostics-reset, and hot-plug regression records remain PASS.
- Final repository checks found no secret-key file/signature, no actual Windows absolute path in target project files, and no tracked ELF/map/log/generated build artifact. The tracked `.hex`/`.bin` files under `tools/sealed_model/tests/vectors/` are intentional public test vectors.

## Hardware acceptance result

All required Phase 4.1B-ST gates passed:

- SAES/DHUK wrap and reuse; GCM empty/37-byte/4/16/64 KiB, AAD, partial block, reinitialization.
- Tag/ciphertext rejection and caller-output zeroization, with observed HAL status/error recorded.
- Initial NOR provisioning, readback, normal-app load, warm reset reuse, complete power removal/reapply reuse.
- The exact fleet-specific `MTFSTEST.MTF`: envelope, immediate model-key wrap/zeroize, both payload chunks, known plaintext, all three RAM-only negative mutations, and FatFs cleanup.
- FullSecure privileged execution plus effective SAES/RNG/XSPI access and DHUK-context persistence. This is an operational attribution check, not a production RIF/lifecycle certification claim.
- Existing STM32 SDMMC IDMA smoke regression.
- Debug warning/error-free build and final ELF/map/log/repository secret scan.

Optional and not performed: wrapped-record copy to a second N657, exhaustive update power-cut injection, anti-rollback, remote/production secure injection, TrustZone split, DMA/shared-key performance work, and certification hardening.

## Official references

- [RM0486: STM32N647/657 reference manual](https://www.st.com/resource/en/reference_manual/dm00769900.pdf)
- [STM32N657 data sheet](https://www.st.com/resource/en/datasheet/stm32n657l0.pdf)
- [UM3451: STM32N6 security guidance](https://www.st.com/resource/en/user_manual/um3451-stm32n6xx-security-guidance-for-sesip-level-3-certification-stmicroelectronics.pdf)
