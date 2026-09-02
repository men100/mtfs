from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from bundle import BundleError, inspect_bundle, parse_bundle


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Inspect Sentinel bundle metadata without dumping model bytes")
    parser.add_argument("bundle", type=Path)
    try:
        args = parser.parse_args(argv)
        result = inspect_bundle(parse_bundle(args.bundle.read_bytes()))
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except (BundleError, OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
