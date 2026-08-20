# STM32N6570-DK fleet-key provisioner

This is the dedicated development provisioner for Phase 4.1B-ST. It accepts a fleet AES-256 key from a trusted local UART/XMODEM session, wraps it with SAES/DHUK, validates the wrapped key, zeroizes the raw buffer, and commits the wrapped blob to the board NOR dual-slot store.

It is not a production secure-injection solution. It does not program OTP, change lifecycle, configure tamper, or lock debug. Do not place the raw key in the repository, command line, descriptor, log, ELF, or map.

## Build

Import the root project plus `Appli` and `FSBL` into STM32CubeIDE, or use the existing headless project names:

```text
mtfs_stm32n6570_dk_key_provision_FSBL
mtfs_stm32n6570_dk_key_provision_Appli
```

The baseline is STM32Cube FW_N6 V1.3.0 and the existing FullSecure LRUN configuration. Load the provisioner through the established development flow. Normal firmware programming erases only the sectors needed by the image and leaves the key slots intact. Do not request a whole-chip erase after provisioning unless loss of the wrapped fleet key is intended.

## Commands

```text
info              show public provider/NOR geometry; no write
verify-nor        load and crypto-validate the current record; no write
provision-xmodem  create the first record; rejects an existing valid record
update-xmodem     explicit inactive-slot update and key-version increment
help              show commands
```

For `provision-xmodem`, use XMODEM-CRC to send a binary file containing exactly 32 key bytes. The receiver accepts a 128-byte or 1 KiB transport block and permits only XMODEM `0x00` or `0x1a` padding after byte 32. It rejects bad CRC/block numbering, non-padding excess, missing or additional blocks, incomplete transfer, and a 60-second start timeout. Classic XMODEM does not transmit the original file length, so an excess suffix made entirely of padding bytes cannot be distinguished from transport padding; control the sender input file length.

Successful provisioning prints generation, key version, slot offset, and operation counts only. It never prints raw or wrapped key bytes. The raw receive buffer is cleared immediately after DHUK wrapping, before NOR programming.

## NOR reservation and recovery

- MX66UW1G45G, 128 MiB, 4 KiB erase, 256-byte program.
- slot A: offset `0x07FFE000` (memory-mapped address `0x77FFE000`).
- slot B: offset `0x07FFF000` (memory-mapped address `0x77FFF000`).
- FSBL/application reserved area ends at `0x0017ffff`.

These are the final two 4 KiB subsectors of the 128 MiB NOR and must be excluded from every firmware image and explicit firmware erase range. Commit is inactive-slot erase, staged program, byte readback, commit word last, then final decode/CRC validation. An interrupted update leaves the previously committed slot usable. A whole-chip erase or loss of both slots requires trusted reprovisioning.

After provisioning, flash the normal application without erasing these two subsectors and run `crypto-consistency`, `crypto-package-test`, and `crypto-negative`. Warm-reset and complete-power-removal reuse must both be recorded before calling the STM32 phase a hardware pass. See [`docs/security/stm32n657-saes-dhuk-spike.md`](../../../../docs/security/stm32n657-saes-dhuk-spike.md) for the full acceptance checklist.
