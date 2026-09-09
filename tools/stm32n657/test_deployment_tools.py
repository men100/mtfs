from __future__ import annotations

import argparse
import unittest
from pathlib import Path

import flash_binary
import make_no_key_trusted_header


class DeploymentToolTests(unittest.TestCase):
    def test_no_key_wording_and_command(self) -> None:
        args = make_no_key_trusted_header.parser().parse_args([
            "--signing-tool", "vendor-tool",
            "--input", "input.bin",
            "--output", "output.bin",
        ])
        built = make_no_key_trusted_header.command(args)
        self.assertIn("-nk", built)
        self.assertEqual(built[built.index("-of") + 1], "0x80000000")
        self.assertNotIn("-la", built)
        self.assertNotIn("sign", make_no_key_trusted_header.__doc__.lower())

    def test_explicit_load_address_uses_la(self) -> None:
        args = make_no_key_trusted_header.parser().parse_args([
            "--signing-tool", "vendor-tool",
            "--input", "input.bin",
            "--output", "output.bin",
            "--load-address", "0x34180000",
        ])
        built = make_no_key_trusted_header.command(args)
        self.assertEqual(built[built.index("-of") + 1], "0x80000000")
        self.assertEqual(built[built.index("-la") + 1], "0x34180000")

    def test_flash_uses_caller_binary_and_address(self) -> None:
        args = argparse.Namespace(programmer=Path("programmer"),
            external_loader=Path("loader.stldr"), binary=Path("firmware.bin"),
            connection="port=SWD", mode="HOTPLUG", access_port="1",
            address="0x70100000")
        built = flash_binary.flash_command(args)
        self.assertEqual(built[-3:], ["firmware.bin", "0x70100000", "-v"])


if __name__ == "__main__":
    unittest.main()
