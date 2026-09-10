#!/usr/bin/env python3
"""Path-neutral STM32CubeProgrammer tool discovery helpers."""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path
from typing import Callable, Iterable, Mapping, Sequence


PROGRAMMER = "STM32_Programmer_CLI.exe"
SIGNING_TOOL = "STM32_SigningTool_CLI.exe"
EXTERNAL_LOADER = "MX66UW1G45G_STM32N6570-DK.stldr"
ENVIRONMENT_ROOT = "STM32CUBE_PROGRAMMER_DIR"


class DiscoveryError(RuntimeError):
    """Raised when a required vendor tool cannot be selected unambiguously."""


def _deduplicate(paths: Iterable[Path]) -> list[Path]:
    result: list[Path] = []
    seen: set[str] = set()
    for path in paths:
        if not path.is_file():
            continue
        resolved = path.resolve()
        key = str(resolved).casefold()
        if key not in seen:
            seen.add(key)
            result.append(resolved)
    return result


def _select(paths: Iterable[Path], source: str, name: str) -> Path | None:
    candidates = _deduplicate(paths)
    if len(candidates) > 1:
        joined = "\n  ".join(str(path) for path in candidates)
        raise DiscoveryError(
            f"multiple {name} candidates from {source}; specify an explicit path:\n  {joined}")
    return candidates[0] if candidates else None


def _under_root(root: Path, name: str) -> list[Path]:
    return [root / name, root / "bin" / name, root / "tools" / "bin" / name]


def _default_standalone_roots(environment: Mapping[str, str]) -> list[Path]:
    roots: list[Path] = []
    for variable in ("ProgramFiles", "ProgramFiles(x86)"):
        value = environment.get(variable)
        if value:
            roots.append(Path(value) / "STMicroelectronics" / "STM32Cube" /
                         "STM32CubeProgrammer" / "bin")
    roots.append(Path("C:/Program Files/STMicroelectronics/STM32Cube/") /
                 "STM32CubeProgrammer/bin")
    return roots


def _default_plugin_roots(environment: Mapping[str, str]) -> list[Path]:
    roots = [Path("C:/ST")]
    for variable in ("ProgramFiles", "ProgramFiles(x86)"):
        value = environment.get(variable)
        if value:
            roots.append(Path(value) / "STMicroelectronics")
    return roots


def _plugin_candidates(roots: Sequence[Path], name: str) -> list[Path]:
    patterns = (
        "STM32CubeIDE_*/STM32CubeIDE/plugins/"
        "com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.*/tools/bin/" + name,
        "STM32CubeIDE/plugins/"
        "com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.*/tools/bin/" + name,
    )
    return [candidate for root in roots for pattern in patterns
            for candidate in root.glob(pattern)]


def resolve_executable(
    name: str,
    explicit: Path | None = None,
    *,
    environment: Mapping[str, str] | None = None,
    which: Callable[..., str | None] = shutil.which,
    standalone_roots: Sequence[Path] | None = None,
    plugin_roots: Sequence[Path] | None = None,
) -> tuple[Path, str]:
    """Resolve one executable in the documented priority order."""
    env = os.environ if environment is None else environment
    if explicit is not None:
        path = explicit.expanduser().resolve()
        if not path.is_file():
            raise DiscoveryError(f"explicit {name} does not exist: {path}")
        return path, "cli"

    configured = env.get(ENVIRONMENT_ROOT)
    if configured:
        root = Path(configured).expanduser()
        selected = _select(_under_root(root, name), ENVIRONMENT_ROOT, name)
        if selected is None:
            raise DiscoveryError(f"{name} not found under {ENVIRONMENT_ROOT}: {root}")
        return selected, "environment"

    found = which(name, path=env.get("PATH"))
    if found:
        return Path(found).resolve(), "PATH"

    roots = (_default_standalone_roots(env) if standalone_roots is None
             else list(standalone_roots))
    selected = _select((candidate for root in roots
                        for candidate in _under_root(root, name)),
                       "standalone installation", name)
    if selected is not None:
        return selected, "standalone"

    plugin_search = (_default_plugin_roots(env) if plugin_roots is None
                     else list(plugin_roots))
    selected = _select(_plugin_candidates(plugin_search, name),
                       "STM32CubeIDE plugins", name)
    if selected is not None:
        return selected, "cubeide-plugin"
    raise DiscoveryError(
        f"{name} not found; use an explicit option or {ENVIRONMENT_ROOT}")


def resolve_loader(
    explicit: Path | None,
    programmer: Path,
    *,
    environment: Mapping[str, str] | None = None,
    standalone_roots: Sequence[Path] | None = None,
    plugin_roots: Sequence[Path] | None = None,
) -> tuple[Path, str]:
    """Resolve the board loader, preferring the selected programmer package."""
    env = os.environ if environment is None else environment
    if explicit is not None:
        path = explicit.expanduser().resolve()
        if not path.is_file():
            raise DiscoveryError(f"explicit external loader does not exist: {path}")
        return path, "cli"

    adjacent = programmer.parent / "ExternalLoader" / EXTERNAL_LOADER
    if adjacent.is_file():
        return adjacent.resolve(), "programmer-package"

    configured = env.get(ENVIRONMENT_ROOT)
    if configured:
        root = Path(configured).expanduser()
        candidates = [parent / "ExternalLoader" / EXTERNAL_LOADER
                      for parent in _under_root(root, PROGRAMMER)]
        selected = _select(candidates, ENVIRONMENT_ROOT, EXTERNAL_LOADER)
        if selected is None:
            raise DiscoveryError(
                f"{EXTERNAL_LOADER} not found under {ENVIRONMENT_ROOT}: {root}")
        return selected, "environment"

    roots = (_default_standalone_roots(env) if standalone_roots is None
             else list(standalone_roots))
    selected = _select((root / "ExternalLoader" / EXTERNAL_LOADER
                        for root in roots), "standalone installation", EXTERNAL_LOADER)
    if selected is not None:
        return selected, "standalone"

    plugin_search = (_default_plugin_roots(env) if plugin_roots is None
                     else list(plugin_roots))
    selected = _select(_plugin_candidates(
        plugin_search, "ExternalLoader/" + EXTERNAL_LOADER),
        "STM32CubeIDE plugins", EXTERNAL_LOADER)
    if selected is not None:
        return selected, "cubeide-plugin"
    raise DiscoveryError(
        f"{EXTERNAL_LOADER} not found; use --external-loader explicitly")


def tool_version(path: Path, timeout: float = 15.0) -> str:
    completed = subprocess.run(
        [str(path), "--version"], check=True, capture_output=True,
        text=True, errors="replace", timeout=timeout, shell=False)
    output = (completed.stdout + completed.stderr).strip()
    return " ".join(output.split())
