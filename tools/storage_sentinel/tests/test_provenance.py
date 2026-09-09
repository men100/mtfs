from __future__ import annotations

import tempfile
import unittest
import json
from pathlib import Path

from provenance import (assert_path_neutral_tree, neutralize_training_paths,
                        normalize_logical_path, relative_provenance_path)
from publish_dataset import publish_capture
from schema import DatasetError


class ProvenancePathTests(unittest.TestCase):
    def test_windows_path_is_canonicalized_relative_to_root(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "datasets" / "card a" / "capture.csv"
            source.parent.mkdir(parents=True)
            source.touch()
            self.assertEqual(relative_provenance_path(source, root),
                             "datasets/card a/capture.csv")

    def test_absolute_and_parent_paths_are_rejected(self) -> None:
        for value in ("C:\\private\\capture.csv", "/private/capture.csv",
                      "datasets/../private.csv", "..\\private.csv"):
            with self.subTest(value=value), self.assertRaises(DatasetError):
                normalize_logical_path(value)

    def test_legacy_absolute_path_requires_explicit_root(self) -> None:
        manifest = {"train_sessions": [{"path": "C:\\work\\data.csv"}],
                    "validation_sessions": [],
                    "validation_pseudo_sessions": [], "heldout_sessions": []}
        with self.assertRaises(DatasetError):
            neutralize_training_paths(manifest, None)

    def test_authenticated_tree_rejects_embedded_absolute_path(self) -> None:
        assert_path_neutral_tree({"path": "datasets/card-a/capture.csv"})
        with self.assertRaises(DatasetError):
            assert_path_neutral_tree({"arguments": "tool -i C:\\work\\model.tflite"})
        with self.assertRaises(DatasetError):
            assert_path_neutral_tree({"arguments": "profile@C:\\work\\profile.json"})
        with self.assertRaises(DatasetError):
            assert_path_neutral_tree({"arguments": "tool -i /tmp/model.tflite"})

    def test_capture_payload_is_unchanged_and_only_source_is_normalized(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "private" / "capture.csv"
            destination = root / "public" / "capture.csv"
            source.parent.mkdir()
            source.write_bytes(b"a,b\r\n1,2\r\n")
            source.with_suffix(".csv.manifest.json").write_text(json.dumps({
                "source": "C:\\private\\raw.log", "session_id": "session-a",
                "content_sha256": "fixed", "capture_context": {"condition": "normal"},
            }), encoding="utf-8")
            report = publish_capture(source, destination, "raw-captures/capture.log")
            self.assertEqual(destination.read_bytes(), source.read_bytes())
            published = json.loads(destination.with_suffix(
                ".csv.manifest.json").read_text(
                encoding="utf-8"))
            self.assertEqual(published["source"], "raw-captures/capture.log")
            self.assertEqual(published["session_id"], "session-a")
            self.assertEqual(report["payload"]["source_sha256"],
                             report["payload"]["published_sha256"])


if __name__ == "__main__":
    unittest.main()
