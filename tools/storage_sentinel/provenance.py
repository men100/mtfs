"""Path-neutral provenance helpers for reproducible release artifacts."""
from __future__ import annotations

import copy
import re
from pathlib import Path, PurePosixPath, PureWindowsPath

from schema import DatasetError


_WINDOWS_ABSOLUTE = re.compile(r"(?:^|[^A-Za-z0-9])(?:[A-Za-z]:[\\/]|\\\\)")
_POSIX_MACHINE_PATH = re.compile(
    r"(?:^|[\s\"'=:@])/(?:home|tmp|Users|var|opt|mnt)/")


def normalize_logical_path(value: str) -> str:
    """Return a canonical relative POSIX path or fail closed.

    論理pathはroot相対のPOSIX表記に限定し、絶対pathとroot外参照を拒否する。
    """
    if not value or PureWindowsPath(value).is_absolute() or \
            PurePosixPath(value).is_absolute():
        raise DatasetError(f"absolute or empty provenance path: {value!r}")
    normalized = value.replace("\\", "/")
    parts = PurePosixPath(normalized).parts
    if not parts or any(part in {"", ".", ".."} for part in parts):
        raise DatasetError(f"unsafe provenance path: {value!r}")
    return PurePosixPath(*parts).as_posix()


def relative_provenance_path(path: Path, root: Path) -> str:
    """Map an existing path to a canonical logical name below an explicit root."""
    resolved = path.resolve()
    resolved_root = root.resolve()
    try:
        relative = resolved.relative_to(resolved_root)
    except ValueError as error:
        raise DatasetError(
            f"provenance input is outside --provenance-root: {resolved}") from error
    return normalize_logical_path(relative.as_posix())


def neutralize_training_paths(manifest: dict, root: Path | None) -> dict:
    """Copy a training manifest and normalize every recorded dataset path."""
    result = copy.deepcopy(manifest)
    for field in ("train_sessions", "validation_sessions",
                  "validation_pseudo_sessions", "heldout_sessions"):
        entries = result.get(field, [])
        if not isinstance(entries, list):
            raise DatasetError(f"invalid training manifest field: {field}")
        for entry in entries:
            if not isinstance(entry, dict) or not isinstance(entry.get("path"), str):
                if field == "heldout_sessions" and not entries:
                    continue
                raise DatasetError(f"missing provenance path in {field}")
            value = entry["path"]
            if PureWindowsPath(value).is_absolute() or PurePosixPath(value).is_absolute():
                if root is None:
                    raise DatasetError(
                        "release provenance contains an absolute path; "
                        "specify --provenance-root")
                entry["path"] = relative_provenance_path(Path(value), root)
            else:
                entry["path"] = normalize_logical_path(value)
    return result


def assert_path_neutral_tree(payload: object, location: str = "provenance") -> None:
    """Reject machine-specific absolute paths anywhere in authenticated metadata."""
    if isinstance(payload, dict):
        for key, value in payload.items():
            assert_path_neutral_tree(value, f"{location}.{key}")
    elif isinstance(payload, list):
        for index, value in enumerate(payload):
            assert_path_neutral_tree(value, f"{location}[{index}]")
    elif isinstance(payload, str):
        if _WINDOWS_ABSOLUTE.search(payload) or payload.startswith("/") or \
                _POSIX_MACHINE_PATH.search(payload):
            raise DatasetError(f"absolute path in {location}")
