from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from bundle import BundleError, atomic_write, build_bundle, parse_bundle, verify_bundle
from schema import DatasetError


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Create deterministic Sentinel bundle v1")
    result.add_argument("--artifact", type=Path, required=True)
    result.add_argument("--canonical-tflite", type=Path, required=True,
                        help="the sole canonical deployed full-int8 TFLite")
    result.add_argument("--output", type=Path, required=True)
    result.add_argument("--no-cpu", action="store_true")
    result.add_argument("--npu-binary", type=Path)
    result.add_argument("--npu-manifest", type=Path)
    result.add_argument("--npu-acceptance", type=Path,
                        help="offline-fixed Neural-ART numeric acceptance contract")
    result.add_argument("--profile-id", type=lambda value: int(value, 0))
    result.add_argument("--accelerator-id", type=lambda value: int(value, 0),
                        help="assert outer accelerator policy ID")
    return result


def main(argv: list[str] | None = None) -> int:
    try:
        args = parser().parse_args(argv)
        bundle, summary = build_bundle(args.artifact, not args.no_cpu,
            args.npu_binary, args.npu_manifest, args.profile_id,
            args.accelerator_id, args.canonical_tflite, args.npu_acceptance)
        parsed = parse_bundle(bundle, summary["target_id"], summary["transport_id"],
                              summary["accelerator_id"])
        verify_bundle(parsed)
        atomic_write(args.output, bundle)
        print(json.dumps(summary, indent=2, sort_keys=True, allow_nan=False))
        return 0
    except (BundleError, DatasetError, OSError, ValueError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
