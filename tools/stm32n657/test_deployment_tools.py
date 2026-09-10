from __future__ import annotations

import argparse
import json
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Callable

import flash_binary
import make_no_key_trusted_header
import tool_discovery


class ToolDiscoveryTests(unittest.TestCase):
    def test_explicit_path_with_spaces(self) -> None:
        with tempfile.TemporaryDirectory(prefix="mtfs tool ") as directory:
            tool = Path(directory) / tool_discovery.SIGNING_TOOL
            tool.write_bytes(b"fake")
            selected, source = tool_discovery.resolve_executable(
                tool_discovery.SIGNING_TOOL, tool)
            self.assertEqual(selected, tool.resolve())
            self.assertEqual(source, "cli")

    def test_environment_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            tool = Path(directory) / "bin" / tool_discovery.PROGRAMMER
            tool.parent.mkdir()
            tool.write_bytes(b"fake")
            selected, source = tool_discovery.resolve_executable(
                tool_discovery.PROGRAMMER, environment={
                    tool_discovery.ENVIRONMENT_ROOT: directory, "PATH": ""})
            self.assertEqual(selected, tool.resolve())
            self.assertEqual(source, "environment")

    def test_path_discovery(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            tool = Path(directory) / tool_discovery.PROGRAMMER
            tool.write_bytes(b"fake")
            selected, source = tool_discovery.resolve_executable(
                tool_discovery.PROGRAMMER, environment={"PATH": "fake"},
                which=lambda _name, path=None: str(tool),
                standalone_roots=[], plugin_roots=[])
            self.assertEqual(selected, tool.resolve())
            self.assertEqual(source, "PATH")

    def test_cubeide_plugin_discovery(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            tool = (Path(directory) / "STM32CubeIDE_2.1.1" / "STM32CubeIDE" /
                    "plugins" /
                    "com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_1" /
                    "tools" / "bin" / tool_discovery.SIGNING_TOOL)
            tool.parent.mkdir(parents=True)
            tool.write_bytes(b"fake")
            selected, source = tool_discovery.resolve_executable(
                tool_discovery.SIGNING_TOOL, environment={"PATH": ""},
                which=lambda _name, path=None: None, standalone_roots=[],
                plugin_roots=[Path(directory)])
            self.assertEqual(selected, tool.resolve())
            self.assertEqual(source, "cubeide-plugin")

    def test_multiple_plugin_candidates_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for version in ("1.0", "2.0"):
                tool = (root / f"STM32CubeIDE_{version}" / "STM32CubeIDE" /
                        "plugins" /
                        f"com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_{version}" /
                        "tools" / "bin" / tool_discovery.SIGNING_TOOL)
                tool.parent.mkdir(parents=True)
                tool.write_bytes(b"fake")
            with self.assertRaises(tool_discovery.DiscoveryError):
                tool_discovery.resolve_executable(
                    tool_discovery.SIGNING_TOOL, environment={"PATH": ""},
                    which=lambda _name, path=None: None, standalone_roots=[],
                    plugin_roots=[root])

    def test_missing_tool_is_rejected(self) -> None:
        with self.assertRaises(tool_discovery.DiscoveryError):
            tool_discovery.resolve_executable(
                tool_discovery.PROGRAMMER, environment={"PATH": ""},
                which=lambda _name, path=None: None, standalone_roots=[],
                plugin_roots=[])

    def test_loader_adjacent_to_programmer(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            programmer = Path(directory) / tool_discovery.PROGRAMMER
            programmer.write_bytes(b"fake")
            loader = Path(directory) / "ExternalLoader" / tool_discovery.EXTERNAL_LOADER
            loader.parent.mkdir()
            loader.write_bytes(b"fake")
            selected, source = tool_discovery.resolve_loader(None, programmer)
            self.assertEqual(selected, loader.resolve())
            self.assertEqual(source, "programmer-package")


class PreparationToolTests(unittest.TestCase):
    def _args(self, *extra: str) -> argparse.Namespace:
        return make_no_key_trusted_header.parser().parse_args([
            "--input", "input.bin", "--output", "output.bin", *extra])

    def test_default_uses_option_flags_without_load_address(self) -> None:
        args = self._args()
        built = make_no_key_trusted_header.command(args)
        self.assertEqual(built[built.index("-of") + 1], "0x80000000")
        self.assertNotIn("-la", built)
        self.assertIn("-align", built)
        self.assertIn("-dump", built)
        self.assertNotIn("sign", make_no_key_trusted_header.__doc__.lower())

    def test_explicit_load_address_uses_la(self) -> None:
        args = self._args("--load-address", "0x34180000")
        built = make_no_key_trusted_header.command(args)
        self.assertEqual(built[built.index("-of") + 1], "0x80000000")
        self.assertEqual(built[built.index("-la") + 1], "0x34180000")

    def test_project_discovers_long_unique_names(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, leaf in (("FSBL", "long_sentinel_FSBL.bin"),
                               ("Appli", "long_sentinel_Appli.bin")):
                release = root / name / "Release"
                release.mkdir(parents=True)
                (release / leaf).write_bytes(b"raw")
            args = make_no_key_trusted_header.parser().parse_args([
                "--project", str(root), "--output-dir", str(root / "out")])
            images = make_no_key_trusted_header.resolve_images(args)
            self.assertEqual([image.role for image in images], ["fsbl", "appli"])
            self.assertEqual(images[0].output.name, "long_sentinel_FSBL-trusted.bin")

    def test_multiple_project_candidates_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("FSBL", "Appli"):
                release = root / name / "Release"
                release.mkdir(parents=True)
                (release / "one.bin").write_bytes(b"raw")
            (root / "FSBL" / "Release" / "two.bin").write_bytes(b"raw")
            args = make_no_key_trusted_header.parser().parse_args([
                "--project", str(root), "--output-dir", str(root / "out")])
            with self.assertRaises(make_no_key_trusted_header.PreparationError):
                make_no_key_trusted_header.resolve_images(args)

    def test_missing_input_is_rejected(self) -> None:
        args = self._args()
        images = make_no_key_trusted_header.resolve_images(args)
        with self.assertRaises(make_no_key_trusted_header.PreparationError):
            make_no_key_trusted_header.prepare_images(
                images, Path("tool"), args)

    def test_existing_output_requires_force(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "in.bin"
            output = root / "out.bin"
            source.write_bytes(b"raw")
            output.write_bytes(b"old")
            args = make_no_key_trusted_header.parser().parse_args([
                "--input", str(source), "--output", str(output)])
            images = make_no_key_trusted_header.resolve_images(args)
            with self.assertRaises(make_no_key_trusted_header.PreparationError):
                make_no_key_trusted_header.prepare_images(images, Path("tool"), args)

    def test_dry_run_does_not_create_output(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "in.bin"
            output = root / "out.bin"
            source.write_bytes(b"raw")
            args = make_no_key_trusted_header.parser().parse_args([
                "--input", str(source), "--output", str(output), "--dry-run"])
            reports = make_no_key_trusted_header.prepare_images(
                make_no_key_trusted_header.resolve_images(args), Path("tool"), args)
            self.assertTrue(reports[0]["dry_run"])
            self.assertFalse(output.exists())

    def test_force_replaces_only_after_success(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "in.bin"
            output = root / "out.bin"
            source.write_bytes(b"raw")
            output.write_bytes(b"old")
            output.chmod(stat.S_IREAD)
            args = make_no_key_trusted_header.parser().parse_args([
                "--input", str(source), "--output", str(output), "--force"])

            def runner(argv: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
                destination = Path(argv[argv.index("-o") + 1])
                destination.write_bytes(b"STM2raw")
                destination.chmod(stat.S_IREAD)
                return subprocess.CompletedProcess(argv, 0)

            make_no_key_trusted_header.prepare_images(
                make_no_key_trusted_header.resolve_images(args), Path("tool"),
                args, runner=runner)
            self.assertEqual(output.read_bytes(), b"STM2raw")
            self.assertNotEqual(output.stat().st_mode & stat.S_IWRITE, 0)

    def test_changed_vendor_payload_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "in.bin"
            output = root / "out.bin"
            source.write_bytes(b"raw")
            args = make_no_key_trusted_header.parser().parse_args([
                "--input", str(source), "--output", str(output)])

            def runner(argv: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
                Path(argv[argv.index("-o") + 1]).write_bytes(b"STM2bad")
                return subprocess.CompletedProcess(argv, 0)

            with self.assertRaises(make_no_key_trusted_header.PreparationError):
                make_no_key_trusted_header.prepare_images(
                    make_no_key_trusted_header.resolve_images(args), Path("tool"),
                    args, runner=runner)
            self.assertFalse(output.exists())

    def test_subprocess_failure_cleans_temporary_output(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "in.bin"
            output = root / "out.bin"
            source.write_bytes(b"raw")
            args = make_no_key_trusted_header.parser().parse_args([
                "--input", str(source), "--output", str(output)])

            def runner(argv: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
                destination = Path(argv[argv.index("-o") + 1])
                destination.write_bytes(b"partial")
                destination.chmod(stat.S_IREAD)
                raise subprocess.CalledProcessError(7, argv)

            with self.assertRaises(subprocess.CalledProcessError):
                make_no_key_trusted_header.prepare_images(
                    make_no_key_trusted_header.resolve_images(args), Path("tool"),
                    args, runner=runner)
            self.assertFalse(output.exists())
            self.assertEqual(list(root.glob(".mtfs-header-*")), [])

    def test_sha256(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "value.bin"
            path.write_bytes(b"abc")
            self.assertEqual(make_no_key_trusted_header.sha256(path),
                             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")


class FlashToolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.layout_path = Path(flash_binary.__file__).with_name("layout.json")

    def _trusted(self, path: Path, size: int = 32) -> None:
        path.write_bytes(flash_binary.TRUSTED_HEADER_MAGIC + b"x" * (size - 4))

    def _modified_layout(
        self, root: Path, update: Callable[[dict[str, object]], None]
    ) -> Path:
        data = json.loads(self.layout_path.read_text(encoding="utf-8"))
        update(data)
        path = root / "layout.json"
        path.write_text(json.dumps(data), encoding="utf-8")
        return path

    def test_layout_matches_implementation_key_slots(self) -> None:
        layout = flash_binary.load_layout(self.layout_path)
        self.assertEqual(layout.flash.address, 0x70000000)
        self.assertEqual(layout.flash.size, 128 * 1024 * 1024)
        self.assertEqual(layout.images["fsbl"].address, 0x70000000)
        self.assertEqual(layout.images["appli"].address, 0x70100000)
        self.assertEqual([item.address for item in layout.prohibited],
                         [0x77FFE000, 0x77FFF000])
        header = (Path(__file__).resolve().parents[2] / "src" / "ports" /
                  "stm32_cube" / "crypto" / "mtfs_stm32_nor_key_store.h")
        source = header.read_text(encoding="utf-8")
        self.assertIn("(UINT32_C(128) * 1024U * 1024U)", source)
        self.assertIn("(UINT32_C(4096))", source)
        self.assertIn(
            "(MTFS_STM32_NOR_BYTES - MTFS_STM32_NOR_KEY_RESERVED_BYTES)", source)
        self.assertIn(
            "(MTFS_STM32_NOR_BYTES - MTFS_STM32_NOR_ERASE_BYTES)", source)

    def test_malformed_layout_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "layout.json"
            path.write_text("{}", encoding="utf-8")
            with self.assertRaises(flash_binary.FlashError):
                flash_binary.load_layout(path)

    def test_address_overflow_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self._modified_layout(Path(directory), lambda data: data[
                "external_flash"].update({"base": "0xfffff000", "size": "0x2000"}))
            with self.assertRaises(flash_binary.FlashError):
                flash_binary.load_layout(path)

    def test_key_slot_overlap_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self._modified_layout(Path(directory), lambda data: data[
                "images"]["appli"].update({"region_size": "0x07f00000"}))
            with self.assertRaises(flash_binary.FlashError):
                flash_binary.load_layout(path)

    def test_missing_key_slot_guard_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self._modified_layout(Path(directory), lambda data: data[
                "prohibited_ranges"].pop())
            with self.assertRaises(flash_binary.FlashError):
                flash_binary.load_layout(path)

    def test_changed_board_address_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self._modified_layout(Path(directory), lambda data: data[
                "images"]["fsbl"].update({"address": "0x70001000"}))
            with self.assertRaises(flash_binary.FlashError):
                flash_binary.load_layout(path)

    def test_project_discovers_uppercase_fsbl_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, leaf in (("FSBL", "firmware_FSBL-trusted.bin"),
                               ("Appli", "firmware_Appli-trusted.bin")):
                release = root / name / "Release"
                release.mkdir(parents=True)
                self._trusted(release / leaf)
            args = flash_binary.parser().parse_args(["--project", str(root)])
            images = flash_binary.resolve_images(
                args, flash_binary.load_layout(self.layout_path))
            self.assertEqual([image.role for image in images], ["fsbl", "appli"])

    def test_oversized_image_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fsbl-trusted.bin"
            self._trusted(path, 0x00100001)
            layout = flash_binary.load_layout(self.layout_path)
            with self.assertRaises(flash_binary.FlashError):
                flash_binary.validate_images([
                    flash_binary.FlashImage("fsbl", path, 0x70000000)], layout)

    def test_missing_trusted_header_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fsbl.bin"
            path.write_bytes(b"not a trusted image")
            layout = flash_binary.load_layout(self.layout_path)
            with self.assertRaises(flash_binary.FlashError):
                flash_binary.validate_images([
                    flash_binary.FlashImage("fsbl", path, 0x70000000)], layout)

    def test_flash_command_uses_caller_paths_and_verify(self) -> None:
        image = flash_binary.FlashImage(
            "appli", Path("directory with spaces/firmware.bin"), 0x70100000)
        built = flash_binary.flash_command(
            Path("programmer"), image, Path("loader.stldr"),
            ["-c", "port=SWD", "mode=HOTPLUG", "ap=1"])
        self.assertEqual(built[-3:], [str(image.path), "0x70100000", "-v"])
        self.assertNotIn("erase", " ".join(built).casefold())

    def test_former_flash_command_api_remains_supported(self) -> None:
        args = argparse.Namespace(programmer=Path("programmer"),
            external_loader=Path("loader.stldr"), binary=Path("firmware.bin"),
            connection="port=SWD", mode="HOTPLUG", access_port="1",
            address="0x70100000")
        built = flash_binary.flash_command(args)
        self.assertEqual(built[-3:], ["firmware.bin", "0x70100000", "-v"])

    def test_multiple_probes_require_selection(self) -> None:
        with self.assertRaises(flash_binary.FlashError):
            flash_binary.select_probe(["one", "two"], None)
        self.assertEqual(flash_binary.select_probe(["one", "two"], "two"), "two")


if __name__ == "__main__":
    unittest.main()
