#!/usr/bin/env python3
"""Generate microT-FS v1 vectors independently of the C++ implementation.

TEST ONLY - NOT FOR PRODUCTION
Requires Python cryptography only when regenerating; runtime tests use checked-in bytes.
"""

import json
import struct
from pathlib import Path

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

ROOT = Path(__file__).resolve().parent
NOTICE = "TEST ONLY - NOT FOR PRODUCTION"
KEY_DOMAIN = b"MTFS-KEY-v1\0"
CHUNK_DOMAIN = b"MTFS-CHUNK-v1\0"


def tlv(t, flags, value):
    raw = struct.pack("<HHI", t, flags, len(value)) + value
    return raw + b"\0" * ((-len(raw)) % 8)


fleet_key_path = ROOT / "fleet_test.key"
fleet_key = fleet_key_path.read_bytes()
if len(fleet_key) != 32:
    raise ValueError(f"{fleet_key_path} must contain exactly 32 bytes")
model_key = bytes(range(0x20, 0x40))
package_id = bytes(range(0x40, 0x50))
model_id = bytes(range(0x50, 0x60))
key_nonce = bytes(range(0x60, 0x6C))
prefix = bytes(range(0x70, 0x78))
payload = bytes((i * 7 + 3) & 0xFF for i in range(5000))
metadata = tlv(1, 1, struct.pack("<I", 0x00010000)) + tlv(4, 0, b"golden")
chunk_size = 4096
chunk_count = (len(payload) + chunk_size - 1) // chunk_size

preamble = bytearray(160)
preamble[0:8] = b"MTFSMOD\0"
struct.pack_into("<HHIIIIHHII", preamble, 8, 1, 0, 160, 160 + len(metadata), 1, 1, 1, 1, 1, 1)
preamble[40:56] = package_id
preamble[56:72] = model_id
struct.pack_into("<QIIIIQQIII", preamble, 72,
                 0x0102030405060708, 0x11, 0x22, 0x33, 0,
                 len(payload), len(payload), chunk_size, chunk_count, len(metadata))
struct.pack_into("<I", preamble, 124, 48)
preamble[128:140] = key_nonce
preamble[140:148] = prefix
manifest = bytes(preamble) + metadata

envelope_combined = AESGCM(fleet_key).encrypt(key_nonce, model_key, KEY_DOMAIN + manifest)
chunks = []
package = bytearray(manifest + envelope_combined)
for index in range(chunk_count):
    plain = payload[index * chunk_size:(index + 1) * chunk_size]
    nonce = prefix + struct.pack("<I", index)
    aad = CHUNK_DOMAIN + manifest + struct.pack("<II", index, len(plain))
    combined = AESGCM(model_key).encrypt(nonce, plain, aad)
    package.extend(combined)
    chunks.append({
        "index": index,
        "plain_length": len(plain),
        "nonce_hex": nonce.hex(),
        "aad_hex": aad.hex(),
        "ciphertext_hex": combined[:-16].hex(),
        "tag_hex": combined[-16:].hex(),
    })

vector = {
    "notice": NOTICE,
    "generator": "Python cryptography AESGCM (independent of mtfs_sealed_host)",
    "fleet_key_source": "fleet_test.key (32 bytes; value intentionally omitted)",
    "model_key_hex": model_key.hex(),
    "package_id_hex": package_id.hex(),
    "model_id_hex": model_id.hex(),
    "key_nonce_hex": key_nonce.hex(),
    "payload_nonce_prefix_hex": prefix.hex(),
    "metadata_hex": metadata.hex(),
    "manifest_hex": manifest.hex(),
    "envelope_ciphertext_hex": envelope_combined[:-16].hex(),
    "envelope_tag_hex": envelope_combined[-16:].hex(),
    "payload_plaintext_hex": payload.hex(),
    "chunks": chunks,
    "expected_failures": [
        {"name": "envelope_tag_bit_flip", "offset": len(manifest) + 47, "xor": 1},
        {"name": "payload_ciphertext_bit_flip", "offset": len(manifest) + 48, "xor": 1},
        {"name": "last_chunk_tag_bit_flip", "offset": len(package) - 1, "xor": 1},
    ],
}

(ROOT / "golden_payload.bin").write_bytes(payload)
(ROOT / "golden_package.mtfs").write_bytes(package)
(ROOT / "golden_package.hex").write_text(
    package.hex() + "\n", encoding="ascii", newline="\n")
(ROOT / "golden_manifest.hex").write_text(
    manifest.hex() + "\n", encoding="ascii", newline="\n")
(ROOT / "golden_vector.json").write_text(
    json.dumps(vector, indent=2) + "\n", encoding="ascii", newline="\n")
print(f"generated {len(package)}-byte package with {chunk_count} chunks")
