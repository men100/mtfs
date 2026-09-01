from __future__ import annotations

import argparse
import copy
import csv
import io
import json
import tempfile
import unittest
from pathlib import Path

import numpy as np

from collect import run as collect_run
from dataset import (Dataset, assert_same_profile, load_dataset, manifest_path,
                     sha256_file, write_dataset)
from model import train_autoencoder
from schema import (HEADER, RAW_HISTOGRAM_BUCKETS, DatasetError, encode_row,
                    feature_schema, parse_lines)
from train import run as train_run


def valid_row(sequence: int = 1) -> dict:
    row = {name: 0 for name in HEADER[1:]}
    for name in ("label", "build_type", "command", "scenario_origin", "stage", "injection_kind"):
        row[name] = "x"
    row.update({
        "feature_schema_version": 1, "size": 1024, "target": 11,
        "transport": 22, "timestamp_us": sequence * 1000, "interval_us": 1000,
        "media_generation": 1, "validity": 0xFF, "flags": 0, "samples": 1,
        "label": "normal", "marker": sequence, "build_type": "Debug",
        "command": "record", "scenario_origin": "natural", "stage": "natural",
        "injection_kind": "none", "sequence": sequence,
    })
    histogram = []
    for operation in ("read", "write", "sync"):
        row[f"{operation}_calls"] = 10
        row[f"{operation}_ok"] = 10
        row[f"{operation}_timing"] = 10
        row[f"{operation}_total_us"] = 1000
        row[f"{operation}_avg_us"] = 100
        histogram.extend([10] + [0] * 21)
    row["histogram_r_w_s"] = histogram
    return row


def csv_text(rows: list[dict]) -> str:
    stream = io.StringIO()
    writer = csv.writer(stream, lineterminator="\n")
    writer.writerow(HEADER)
    for row in rows:
        values = [HEADER[0]]
        for name in HEADER[1:]:
            value = row[name]
            values.append(":".join(str(v) for v in value) if isinstance(value, list) else value)
        writer.writerow(values)
    return stream.getvalue()


class SchemaTests(unittest.TestCase):
    def test_histogram_contract_matches_firmware(self):
        row = valid_row()
        self.assertEqual(len(row["histogram_r_w_s"]), 66)
        self.assertEqual(RAW_HISTOGRAM_BUCKETS, 66)
        self.assertEqual(feature_schema()["raw_histogram"], {
            "operations": 3,
            "buckets_per_operation": 22,
            "serialized_buckets": 66,
            "feature_groups": [[0, 3], [4, 7], [8, 11], [12, 14], [15, 21]],
        })
        self.assertEqual(len(parse_lines(csv_text([row]).splitlines(True))), 1)
        row["histogram_r_w_s"] = row["histogram_r_w_s"][:48]
        with self.assertRaisesRegex(DatasetError, "needs 66 buckets"):
            parse_lines(csv_text([row]).splitlines(True))

    def test_metadata_does_not_enter_model_input(self):
        first = valid_row()
        second = copy.deepcopy(first)
        second.update({"scenario_origin": "injected", "stage": "strong", "severity": 3,
                       "injection_rate_permille": 200, "random_seed": 999,
                       "target": 99, "sequence": 77, "media_generation": 8})
        self.assertEqual(encode_row(first), encode_row(second))
        self.assertEqual(len(encode_row(first)), 24)

    def test_hard_fault_never_enters_ai(self):
        row = valid_row()
        row["io_error"] = 1
        with self.assertRaises(DatasetError):
            encode_row(row)

    def test_malformed_inputs(self):
        text = csv_text([valid_row()])
        cases = [
            text + ",truncated\n",
            text.splitlines()[0] + "\n" + text,
            text.replace(",1000,", ",NaN,", 1),
            text.replace(",1024,", ",18446744073709551616,", 1),
        ]
        for case in cases:
            with self.subTest(case=case[-50:]), self.assertRaises(DatasetError):
                parse_lines(case.splitlines(True))

    def test_mixed_profile_and_sequence_are_rejected(self):
        first, second = valid_row(1), valid_row(2)
        second["target"] = 12
        with self.assertRaisesRegex(DatasetError, "mixed target"):
            parse_lines(csv_text([first, second]).splitlines(True))
        second = valid_row(3)
        with self.assertRaisesRegex(DatasetError, "sequence discontinuity"):
            parse_lines(csv_text([first, second]).splitlines(True))
        second = valid_row(2)
        second["transport"] = 23
        with self.assertRaisesRegex(DatasetError, "mixed transport"):
            parse_lines(csv_text([first, second]).splitlines(True))

    def test_mixed_build_types_are_rejected(self):
        first, second = valid_row(1), valid_row(2)
        second["build_type"] = "Release"
        with self.assertRaisesRegex(DatasetError, "mixed build_type"):
            parse_lines(csv_text([first, second]).splitlines(True))
        first_dataset = Dataset(Path("debug.csv"), [first], {"session_id": "a"})
        second_dataset = Dataset(Path("release.csv"), [second], {"session_id": "b"})
        with self.assertRaisesRegex(DatasetError, "mixed build_type"):
            assert_same_profile([first_dataset, second_dataset])


class CollectorTests(unittest.TestCase):
    def test_collect_manifest_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "uart.log"
            output = root / "session.csv"
            source.write_text("# boot\n" + csv_text([valid_row()]), encoding="utf-8")
            args = argparse.Namespace(input=source, serial_port=None, baud=115200,
                output=output, session_id="session-a", card_id="card-a",
                target_name="test", transport_name="test", firmware_commit="abc",
                command="record", expected_rows=2, partial=False)
            manifest = collect_run(args)
            self.assertTrue(manifest["partial"])
            self.assertEqual(load_dataset(output).session_id, "session-a")
            self.assertTrue(manifest_path(output).exists())
            with self.assertRaisesRegex(DatasetError, "refusing overwrite"):
                collect_run(args)


class ModelTests(unittest.TestCase):
    def test_training_is_deterministic(self):
        rng = np.random.default_rng(7)
        values = rng.normal(size=(32, 24))
        first = train_autoencoder(values, [24, 12, 4, 12, 24], 17, epochs=3)
        second = train_autoencoder(values, [24, 12, 4, 12, 24], 17, epochs=3)
        for left, right in zip(first.weights + first.biases, second.weights + second.biases):
            np.testing.assert_array_equal(left, right)

    def test_end_to_end_artifacts_are_reproducible(self):
        def save(path: Path, rows: list[dict], session: str) -> None:
            write_dataset(path, rows)
            manifest_path(path).write_text(json.dumps({
                "session_id": session, "card_id": "test-card", "partial": False,
                "row_count": len(rows), "dataset_sha256": sha256_file(path),
            }), encoding="utf-8")

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            normal_paths = []
            for session_index in range(3):
                rows = []
                for sequence in range(1, 21):
                    row = valid_row(sequence)
                    for operation in ("read", "write", "sync"):
                        row[f"{operation}_avg_us"] += session_index * 3 + sequence % 5
                    rows.append(row)
                path = root / f"normal-{session_index}.csv"
                save(path, rows, f"session-{session_index}")
                normal_paths.append(path)
            delay_rows = []
            sequence = 1
            for stage, severity in (("baseline", 0), ("light", 1),
                                    ("medium", 2), ("strong", 3), ("recovery", 0)):
                for _ in range(5):
                    row = valid_row(sequence)
                    row.update({"command": "pseudo-collect-delay-ramp",
                                "scenario_origin": "injected", "stage": stage,
                                "severity": severity, "injection_kind": "delay"})
                    for operation in ("read", "write", "sync"):
                        row[f"{operation}_avg_us"] += severity * 1000
                    delay_rows.append(row)
                    sequence += 1
            delay_path = root / "delay.csv"
            save(delay_path, delay_rows, "delay-session")
            outputs = []
            for suffix in ("a", "b"):
                output = root / f"artifact-{suffix}"
                args = argparse.Namespace(dataset=normal_paths,
                    evaluation_dataset=[delay_path], output_dir=output, seed=23,
                    epochs=3, minimum_session_frames=20)
                result = train_run(args)
                self.assertEqual(result["topology"], [24, 12, 4, 12, 24])
                self.assertTrue((output / "evaluation_report.json").exists())
                outputs.append(output)
            for name in ("model.json", "normalization.json", "threshold.json",
                         "baselines.json", "training_manifest.json", "test_vectors.json",
                         "evaluation_report.json", "artifact_index.json"):
                self.assertEqual((outputs[0] / name).read_bytes(),
                                 (outputs[1] / name).read_bytes(), name)
            vectors = json.loads((outputs[0] / "test_vectors.json").read_text())
            self.assertIsNotNone(vectors["positive"])
            training = json.loads((outputs[0] / "training_manifest.json").read_text())
            self.assertEqual(training["evaluation_datasets"][0]["session_id"],
                             "delay-session")
            index = json.loads((outputs[0] / "artifact_index.json").read_text())
            self.assertEqual(index["evaluation_dataset_sha256"][str(delay_path)],
                             sha256_file(delay_path))


if __name__ == "__main__":
    unittest.main()
