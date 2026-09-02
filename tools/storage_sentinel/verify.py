from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from bundle import (MODEL_FORMAT_SENTINEL_BUNDLE_V1, BundleError, parse_bundle,
                    verify_bundle)


def optional_int(value: str) -> int:
    return int(value, 0)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Verify Sentinel bundle structure, hashes, and CPU golden vectors")
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--expected-target", type=optional_int)
    parser.add_argument("--expected-transport", type=optional_int)
    parser.add_argument("--expected-accelerator", type=optional_int)
    parser.add_argument("--expected-model-format", type=optional_int,
                        default=MODEL_FORMAT_SENTINEL_BUNDLE_V1)
    try:
        args = parser.parse_args(argv)
        parsed = parse_bundle(args.bundle.read_bytes(), args.expected_target,
            args.expected_transport, args.expected_accelerator,
            args.expected_model_format)
        print(json.dumps(verify_bundle(parsed), indent=2, sort_keys=True))
        return 0
    except (BundleError, OSError, ValueError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
