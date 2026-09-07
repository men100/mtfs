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
from baseline_v2 import (median_baseline, relative_vector, scale_relative,
                         select_policy)
from dataset import (Dataset, assert_same_profile, card_identity, load_dataset,
                     manifest_path, sha256_file, write_dataset)
from evaluate_st_numerical_v3 import evaluate_corpus
from model import train_autoencoder
from schema import (HEADER, HISTOGRAM_FEATURE_GROUPS, RAW_HISTOGRAM_BUCKETS,
                     DatasetError, _permille, canonical_json_sha256, encode_row,
                     feature_schema, parse_lines, schema_canonical_hash)
from st_numerical_v3 import (HashStream, _unique_operational, _unique_stress,
                             derive_seed)
from train import (_matrix, _split_sessions, run as train_run)


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
    def test_permille_reference_boundaries(self):
        maximum = (1 << 64) - 1
        self.assertEqual(_permille(0, maximum), 0)
        self.assertEqual(_permille(1, 2000), 1)
        self.assertEqual(_permille(1999, 2000), 1000)
        self.assertEqual(_permille(maximum, maximum), 1000)
        self.assertEqual(_permille(maximum // 3, maximum), 333)

    def test_histogram_contract_matches_firmware(self):
        row = valid_row()
        self.assertEqual(len(row["histogram_r_w_s"]), 66)
        self.assertEqual(RAW_HISTOGRAM_BUCKETS, 66)
        self.assertEqual(feature_schema()["raw_histogram"], {
            "operations": 3,
            "buckets_per_operation": 22,
            "serialized_buckets": 66,
            "feature_group_bounds": "inclusive",
            "feature_groups": [[0, 3], [4, 7], [8, 11], [12, 14], [15, 21]],
        })
        groups = feature_schema()["raw_histogram"]["feature_groups"]
        self.assertEqual(tuple(tuple(group) for group in groups),
                         HISTOGRAM_FEATURE_GROUPS)
        covered = [bucket for first, last in groups
                   for bucket in range(first, last + 1)]
        self.assertEqual(covered, list(range(22)))
        histogram_feature_names = [item["name"] for item in feature_schema()["features"]
                                   if "_hist_" in item["name"]]
        expected_suffixes = [f"hist_{first}_{last}_permille"
                             for first, last in groups]
        self.assertEqual(histogram_feature_names,
                         [f"{operation}_{suffix}" for operation in ("read", "write", "sync")
                          for suffix in expected_suffixes])
        for bucket in range(22):
            encoded_row = valid_row()
            histogram = []
            for _ in ("read", "write", "sync"):
                operation_buckets = [0] * 22
                operation_buckets[bucket] = 10
                histogram.extend(operation_buckets)
            encoded_row["histogram_r_w_s"] = histogram
            vector = encode_row(encoded_row)
            expected_group = next(index for index, (first, last) in enumerate(groups)
                                  if first <= bucket <= last)
            for operation_index in range(3):
                self.assertEqual(vector[operation_index * 7 + 2:
                                        operation_index * 7 + 7],
                                 [1000 if index == expected_group else 0
                                  for index in range(5)])
        self.assertEqual(len(parse_lines(csv_text([row]).splitlines(True))), 1)
        row["histogram_r_w_s"] = row["histogram_r_w_s"][:48]
        with self.assertRaisesRegex(DatasetError, "needs 66 buckets"):
            parse_lines(csv_text([row]).splitlines(True))

    def test_canonical_schema_hash_is_deterministic(self):
        schema = feature_schema()
        reversed_schema = dict(reversed(list(schema.items())))
        self.assertEqual(canonical_json_sha256(schema),
                         canonical_json_sha256(reversed_schema))
        reparsed = json.loads(json.dumps(schema, indent=4, ensure_ascii=False))
        self.assertEqual(canonical_json_sha256(reparsed), schema_canonical_hash())
        with self.assertRaises(ValueError):
            canonical_json_sha256({"invalid": float("nan")})

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
            self.assertEqual(manifest["feature_schema_canonical_sha256"],
                             schema_canonical_hash())
            self.assertEqual(load_dataset(output).session_id, "session-a")
            self.assertTrue(manifest_path(output).exists())
            with self.assertRaisesRegex(DatasetError, "refusing overwrite"):
                collect_run(args)

    def test_baseline_v2_capture_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "uart.log"
            output = root / "session.csv"
            rows = [valid_row(index) for index in range(1, 9)]
            source.write_text(csv_text(rows), encoding="utf-8")
            args = argparse.Namespace(input=source, serial_port=None, baud=115200,
                output=output, session_id="ra-card-a-normal-01", card_id="card-a",
                target_name="EK-RA8P1", transport_name="SPI",
                firmware_commit="abc", command="record", expected_rows=8,
                partial=False, capture_contract="baseline-relative-v2",
                fat_type="FAT32", allocation_unit=4096,
                power_cycle_id="power-01", mount_session_id="mount-01",
                condition="normal", dataset_role="training-candidate",
                baseline_window_start=1, baseline_window_count=8,
                capture_profile="generic", card_manufacturer=None,
                card_capacity=None)
            manifest = collect_run(args)
            self.assertEqual(manifest["format"],
                             "mtfs-sentinel-dataset-manifest-v2")
            self.assertEqual(manifest["capture_context"]["allocation_unit_bytes"],
                             4096)
            self.assertEqual(manifest["capture_context"]["baseline_windows"],
                             {"first_sequence": 1, "count": 8})
            self.assertNotIn("capture_profile", manifest["capture_context"])
            self.assertNotIn("card_manufacturer", manifest["capture_context"])
            self.assertNotIn("card_capacity", manifest["capture_context"])

    def test_st_release_idma_capture_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "uart.log"
            output = root / "session.csv"
            rows = [valid_row(index) for index in range(1, 33)]
            for row in rows:
                row["target"] = 0x53544E36
                row["transport"] = 0x49444D41
                row["build_type"] = "Release"
            source.write_text(csv_text(rows), encoding="utf-8")
            args = argparse.Namespace(input=source, serial_port=None, baud=115200,
                output=output, session_id="st-card-a-normal-01",
                card_id="st-card-a", card_manufacturer="Lexar",
                card_capacity="4GB", target_name="STM32N6570-DK",
                transport_name="SDMMC-IDMA", firmware_commit="abc",
                command="record", expected_rows=32, partial=False,
                capture_contract="baseline-relative-v2", fat_type="FAT32",
                allocation_unit=4096, power_cycle_id="power-01",
                mount_session_id="mount-01", condition="normal",
                dataset_role="training-candidate", baseline_window_start=1,
                baseline_window_count=32, capture_profile="st-release-idma-v2")
            manifest = collect_run(args)
            context = manifest["capture_context"]
            self.assertEqual(context["capture_profile"], "st-release-idma-v2")
            self.assertEqual(context["card_manufacturer"], "Lexar")
            self.assertEqual(context["card_capacity"], "4GB")
            self.assertEqual(context["workload_status_audit"],
                             {"observed": 0, "nonzero": 0})

            args.output = root / "workload-error.csv"
            source.write_text(
                "# workload-perf marker=1 elapsed_us=1 mtfs=-3\n" +
                csv_text(rows), encoding="utf-8")
            with self.assertRaisesRegex(DatasetError,
                                        "nonzero workload status: 1/1"):
                collect_run(args)

            args.output = root / "debug.csv"
            args.capture_profile = "st-release-idma-v2"
            for row in rows:
                row["build_type"] = "Debug"
            source.write_text(csv_text(rows), encoding="utf-8")
            with self.assertRaisesRegex(DatasetError, "requires STM32N6570-DK"):
                collect_run(args)


class BaselineV2Tests(unittest.TestCase):
    def test_median_relative_scaling_and_ood(self):
        vectors = [[100 + sample] * 24 for sample in range(8)]
        vectors[0] = [10000] * 24
        baseline = median_baseline(vectors, 8)
        self.assertEqual(baseline, [105] * 24)
        raw = baseline.copy()
        raw[0] = 116
        raw[1] = 97
        relative = relative_vector(raw, baseline, (1, 1, 1))
        self.assertEqual(relative[0], 105)
        self.assertEqual(relative[1], -8)
        scaled, mask, ood = scale_relative(relative, [16] * 24,
                                            (1 << 24) - 1, 0)
        self.assertEqual(scaled[:2], [105, -8])
        self.assertEqual(mask, 0)
        self.assertFalse(ood)
        relative[0] = 1000000
        scaled, mask, ood = scale_relative(relative, [16] * 24,
                                            (1 << 24) - 1, 0)
        self.assertEqual(scaled[0], 127)
        self.assertEqual(mask, 1)
        self.assertTrue(ood)

    def test_validation_only_policy_scope_and_training_card_gate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)

            def save(name: str, card: str, role: str, condition: str,
                     rows: list[dict]) -> Path:
                path = root / f"{name}.csv"
                write_dataset(path, rows)
                manifest_path(path).write_text(json.dumps({
                    "session_id": name, "card_id": card,
                    "row_count": len(rows), "dataset_sha256": sha256_file(path),
                    "command": rows[0]["command"],
                    "capture_context": {
                        "contract": "baseline-relative-v2",
                        "operator_card_id": card,
                        "dataset_role": role,
                        "condition": condition,
                    },
                }), encoding="utf-8")
                return path

            training = []
            for card in ("card-a", "card-b"):
                rows = [valid_row(index) for index in range(1, 41)]
                for row in rows:
                    row["build_type"] = "Release"
                    if row["sequence"] > 32:
                        row["read_avg_us"] = 180
                        row["read_total_us"] = 1800
                training.append(save(f"{card}-normal", card,
                                     "training-candidate", "normal", rows))

            validation_rows = [valid_row(index) for index in range(1, 41)]
            for row in validation_rows:
                row["build_type"] = "Release"
            validation = save("card-c-normal", "card-c",
                              "validation-candidate", "normal", validation_rows)

            pseudo_rows = [valid_row(index) for index in range(1, 41)]
            for row in pseudo_rows:
                row["build_type"] = "Release"
                row["command"] = "pseudo-collect-delay-ramp"
                row["scenario_origin"] = "injected"
                row["stage"] = "baseline" if row["sequence"] <= 32 else "medium"
                if row["sequence"] > 32:
                    row["read_avg_us"] = 200
                    row["read_total_us"] = 2000
            pseudo = save("card-c-pseudo", "card-c",
                          "validation-candidate", "pseudo", pseudo_rows)

            validation_only = select_policy(
                training, [validation], [], [pseudo], (32,),
                "validation-only", 2)
            combined = select_policy(training, [validation], [], [pseudo], (32,))
            self.assertEqual(validation_only["policy"]["selection_scope"],
                             "validation-only")
            self.assertEqual(validation_only["policy"]["training_card_ids"],
                             ["card-a", "card-b"])
            self.assertEqual(validation_only["candidate_comparison"][0]
                             ["selection_objective_sum_active_normal_p95_abs"], 0)
            self.assertGreater(combined["candidate_comparison"][0]
                               ["selection_objective_sum_active_normal_p95_abs"], 0)
            with self.assertRaisesRegex(DatasetError,
                                        "exactly 3 identified training cards"):
                select_policy(training, [validation], [], [pseudo], (32,),
                              "validation-only", 3)


class StNumericalV3Tests(unittest.TestCase):
    class FakeModel:
        input_scale = 0.0625
        input_zero_point = 0

    def test_seed_derivation_and_stream_are_deterministic(self):
        identity = "12" * 32
        first = derive_seed(identity, "operational")
        second = derive_seed(identity, "operational")
        self.assertEqual(first, second)
        self.assertNotEqual(first, derive_seed(identity, "stress"))
        left, right = HashStream(first), HashStream(first)
        self.assertEqual(left.take(257), right.take(257))

    def test_operational_and_stress_corpora_are_unique_and_disjoint(self):
        model = self.FakeModel()
        active = [0, 4, 5, 6, 7, 12, 13, 14]
        natural = np.zeros(24, dtype=np.int8)
        light = natural.copy(); light[4] = 8
        medium = natural.copy(); medium[5] = 24
        strong = natural.copy(); strong[6] = 48
        threshold = natural.copy(); threshold[7] = 3
        pools = {"natural": [natural], "light": [light],
                 "medium": [medium], "strong": [strong],
                 "threshold": [threshold]}
        known = {bytes(24)}
        seed = derive_seed("34" * 32, "operational")
        first = _unique_operational(model, 140, seed, known, pools, active)
        second = _unique_operational(model, 140, seed, known, pools, active)
        for left, right in zip(first, second):
            if isinstance(left, list):
                self.assertEqual(left, right)
            else:
                np.testing.assert_array_equal(left, right)
        operational, operational_q4, labels = first
        self.assertEqual(len(set(map(bytes, operational))), 140)
        self.assertFalse(set(map(bytes, operational)) & known)
        self.assertEqual(set(labels), {"natural-near", "pseudo-light-near",
            "pseudo-medium-near", "pseudo-strong-near", "threshold-near",
            "saturation-preboundary", "ood-preboundary"})
        inactive = [index for index in range(24) if index not in active]
        np.testing.assert_array_equal(operational_q4[:, inactive], 0)

        stress, _, stress_labels = _unique_stress(model, 160,
            derive_seed("34" * 32, "stress"),
            known | set(map(bytes, operational)), active)
        self.assertEqual(len(set(map(bytes, stress))), 160)
        self.assertFalse(set(map(bytes, stress)) & set(map(bytes, operational)))
        self.assertIn("active-feature-min-max-zero-pm1", stress_labels)
        self.assertIn("single-feature-perturbation", stress_labels)
        self.assertIn("multiple-feature-combination", stress_labels)
        self.assertIn("deterministic-pseudo-random", stress_labels)

    def test_evaluator_requires_repeatable_target_identical_captures(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            corpus, expected = root / "corpus", root / "expected"
            corpus.mkdir(); expected.mkdir()
            inputs = np.zeros((2, 24), dtype=np.int8)
            q4 = np.zeros_like(inputs)
            raw = np.zeros_like(inputs)
            np.savez(corpus / "operational_inputs_int8.npz", m_inputs_1=inputs)
            np.save(corpus / "operational_input_q4.npy", q4)
            np.save(expected / "operational_expected_raw_int8.npy", raw)
            np.save(expected / "operational_expected_q4.npy", q4)
            np.save(expected / "operational_expected_score_q8.npy",
                    np.zeros(2, dtype=np.uint64))
            np.save(expected / "operational_score_interval_q8.npy",
                    np.asarray([[0, 1], [0, 1]], dtype=np.uint64))
            np.save(expected / "operational_expected_decision.npy",
                    np.zeros(2, dtype=np.bool_))
            np.save(root / "target.npy", raw)

            def capture(path: Path, npu: np.ndarray) -> None:
                np.savez(path, tflite_inputs_int8=inputs,
                         common_q4_inputs=q4, tflite_outputs_int8=raw,
                         npu_outputs_int8=npu)

            capture(root / "run1.npz", raw)
            capture(root / "run2.npz", raw)
            core = {
                "decision_policy": {"threshold_q8": 1},
                "fixed_limits": {
                    "maximum_vendor_raw_int8_error": 2,
                    "maximum_common_q4_output_error": 1,
                },
            }
            model = type("Model", (), {"output_scale": 0.0625,
                                         "output_zero_point": 0})()
            result = evaluate_corpus("operational", core, model, corpus,
                expected, root / "target.npy", root / "run1.npz",
                root / "run2.npz")
            self.assertEqual(result["status"], "PASS")
            self.assertEqual(result["repeatability_failures"], 0)
            self.assertEqual(result["target_airunner_mismatches"], 0)

            changed = raw.copy(); changed[0, 0] = 1
            capture(root / "run3.npz", changed)
            result = evaluate_corpus("operational", core, model, corpus,
                expected, root / "target.npy", root / "run1.npz",
                root / "run3.npz")
            self.assertEqual(result["status"], "STOP")
            self.assertEqual(result["repeatability_failures"], 1)


class ModelTests(unittest.TestCase):
    def test_card_identity_metadata(self):
        def dataset(session: str, card: str) -> Dataset:
            return Dataset(Path(f"{session}.csv"), [valid_row()],
                           {"session_id": session, "card_id": card})

        self.assertEqual(card_identity([dataset("a", "unspecified")]), {
            "status": "unspecified", "count_known": False,
            "identified_card_count": 0, "identified_card_ids": [],
        })
        self.assertEqual(card_identity([dataset("a", "card-a"),
                                        dataset("b", "card-a")]), {
            "status": "complete", "count_known": True,
            "identified_card_count": 1, "identified_card_ids": ["card-a"],
        })
        self.assertEqual(card_identity([dataset("a", "card-b"),
                                        dataset("b", "card-a")]), {
            "status": "complete", "count_known": True,
            "identified_card_count": 2,
            "identified_card_ids": ["card-a", "card-b"],
        })
        self.assertEqual(card_identity([dataset("a", "card-a"),
                                        dataset("b", "unspecified")]), {
            "status": "partial", "count_known": False,
            "identified_card_count": 1, "identified_card_ids": ["card-a"],
        })

    def test_training_is_deterministic(self):
        rng = np.random.default_rng(7)
        values = rng.normal(size=(32, 24))
        first = train_autoencoder(values, [24, 12, 4, 12, 24], 17, epochs=3)
        second = train_autoencoder(values, [24, 12, 4, 12, 24], 17, epochs=3)
        for left, right in zip(first.weights + first.biases, second.weights + second.biases):
            np.testing.assert_array_equal(left, right)

    def test_injected_data_is_rejected_from_training(self):
        row = valid_row()
        row.update({"command": "pseudo-collect-delay-ramp",
                    "scenario_origin": "injected", "stage": "light",
                    "severity": 1, "injection_kind": "delay"})
        dataset = Dataset(Path("injected.csv"), [row],
                          {"session_id": "injected", "card_id": "card-a",
                           "partial": False})
        with self.assertRaisesRegex(DatasetError, "forbidden in training"):
            _matrix([dataset], 1)

    def test_session_split_contract_is_stable(self):
        datasets = [Dataset(Path(f"{index}.csv"), [valid_row()],
                            {"session_id": f"session-{index}",
                             "card_id": "card-a"})
                    for index in range(1, 4)]
        train, validation, test = _split_sessions(datasets, 4303)
        self.assertEqual([item.session_id for item in train], ["session-3"])
        self.assertEqual(validation.session_id, "session-1")
        self.assertEqual(test.session_id, "session-2")

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
            self.assertEqual(training["card_identity"]["status"], "complete")
            self.assertEqual(training["feature_schema_canonical_sha256"],
                             schema_canonical_hash())
            index = json.loads((outputs[0] / "artifact_index.json").read_text())
            self.assertEqual(index["evaluation_dataset_sha256"][str(delay_path)],
                             sha256_file(delay_path))
            self.assertEqual(index["feature_schema_canonical_sha256"],
                             schema_canonical_hash())
            artifact_schema = json.loads((outputs[0] / "feature_schema_v1.json").read_text())
            self.assertEqual(canonical_json_sha256(artifact_schema),
                             schema_canonical_hash())


if __name__ == "__main__":
    unittest.main()
