from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from bundle import parse_bundle


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare Python bundle vectors with portable C inference")
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--bundle", type=Path, action="append", required=True)
    args = parser.parse_args()
    checked = 0
    results = []
    for path in args.bundle:
        parsed = parse_bundle(path.read_bytes())
        for name, vector in parsed.provenance.get("test_vectors", {}).items():
            command = [str(args.runner), str(path), str(parsed.target),
                       str(parsed.transport), str(parsed.accelerator),
                       *[str(value) for value in vector["input_q4"]]]
            completed = subprocess.run(command, check=False, text=True,
                                       stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE)
            if completed.returncode != 0:
                raise RuntimeError(f"C runner failed for {path} {name}: {completed.stderr}")
            actual = json.loads(completed.stdout)
            expected = {"score_q8": vector["score_q8"],
                        "threshold_q8": parsed.threshold,
                        "anomaly": 1 if vector["anomaly"] else 0,
                        "output_q4": vector["output_q4"]}
            if actual != expected:
                raise RuntimeError(f"Python/C mismatch for {path} {name}")
            checked += 1
        results.append({"bundle": str(path), "vectors": len(
            parsed.provenance.get("test_vectors", {}))})
    print(json.dumps({"status": "equivalent", "vectors_checked": checked,
                      "bundles": results}, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
