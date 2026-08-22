#!/usr/bin/env python3
"""Generate deterministic binary fuzz/target replay inputs from mutation_corpus.txt."""

from pathlib import Path
import sys


def main() -> int:
    root = Path(__file__).resolve().parent
    golden = Path(sys.argv[1]) if len(sys.argv) > 1 else (
        root / "../../tools/sealed_model/tests/vectors/golden_package.mtfs")
    output = Path(sys.argv[2]) if len(sys.argv) > 2 else root / "corpus"
    source = golden.read_bytes()
    output.mkdir(parents=True, exist_ok=True)
    (output / "golden_package.mtfs").write_bytes(source)
    for line in (root / "mutation_corpus.txt").read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, offset_text, operation, value_text = line.split()
        offset = int(offset_text, 0)
        value = int(value_text, 16 if operation == "xor" else 0)
        data = bytearray(source)
        if operation == "xor":
            data[offset] ^= value
        elif operation == "zero":
            data[offset:offset + value] = bytes(value)
        elif operation == "fill_ff":
            data[offset:offset + value] = b"\xff" * value
        else:
            raise ValueError(f"unknown mutation operation: {operation}")
        (output / f"{name}.mtfs").write_bytes(data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
