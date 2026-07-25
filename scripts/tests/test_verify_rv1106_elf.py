#!/usr/bin/env python3
"""Offline tests for the RV1106 ELF ABI verifier."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
VERIFIER = REPOSITORY_ROOT / "scripts" / "probes" / "verify_rv1106_elf.py"


FAKE_READELF = r'''#!/usr/bin/env python3
import os
import sys

scenario = os.environ.get("FAKE_READELF_SCENARIO", "compatible")
arguments = sys.argv[1:]
input_path = arguments[-1]

if scenario == "tool_error" and "-h" in arguments:
    print("cannot inspect " + input_path, file=sys.stderr)
    raise SystemExit(9)

if "-h" in arguments:
    if scenario == "abi_mismatch":
        print("Class: ELF64")
        print("Data: 2's complement, little endian")
        print("Machine: AArch64")
        print("Flags: 0x400, Version4 EABI, soft-float ABI")
    else:
        print("Class: ELF32")
        if scenario == "big_endian":
            print("Data: 2's complement, big endian")
        else:
            print("Data: 2's complement, little endian")
        print("Machine: ARM")
        print("Flags: 0x5000400, Version5 EABI, hard-float ABI")
elif "-l" in arguments:
    if scenario == "abi_mismatch":
        print("[Requesting program interpreter: /lib/ld-linux-armhf.so.3]")
    else:
        print("[Requesting program interpreter: /lib/ld-uClibc.so.0]")
elif "-d" in arguments:
    print("0x00000001 (NEEDED) Shared library: [libstdc++.so.6]")
    if scenario == "abi_mismatch":
        print("0x00000001 (NEEDED) Shared library: [libc.so.6]")
    else:
        print("0x00000001 (NEEDED) Shared library: [libc.so.0]")
    if scenario == "rpath":
        print("0x0000001d (RUNPATH) Library runpath: [/home/private/build/output]")
elif "--version-info" in arguments:
    if scenario != "no_versions":
        print("Name: GLIBCXX_3.4.9")
        if scenario == "glibcxx_newer":
            print("Name: GLIBCXX_3.4.30")
else:
    raise SystemExit(3)
'''


class VerifyRv1106ElfTest(unittest.TestCase):
    def run_verifier(self, scenario: str, max_glibcxx=None):
        with tempfile.TemporaryDirectory(prefix="boompi-elf-verifier-") as temporary:
            root = Path(temporary)
            elf = root / "private-build" / "boompi-client"
            elf.parent.mkdir()
            elf.write_bytes(b"\x7fELF fixture without embedded host paths\0")
            fake_readelf = root / "fake_readelf.py"
            fake_readelf.write_text(textwrap.dedent(FAKE_READELF), encoding="utf-8")

            command = [
                sys.executable,
                str(VERIFIER),
                "--readelf",
                str(fake_readelf),
            ]
            if max_glibcxx is not None:
                command.extend(("--max-glibcxx", max_glibcxx))
            command.append(str(elf))

            environment = os.environ.copy()
            environment["FAKE_READELF_SCENARIO"] = scenario
            completed = subprocess.run(
                command,
                check=False,
                capture_output=True,
                text=True,
                env=environment,
            )
            document = json.loads(completed.stdout)
            self.assertNotIn(str(root), completed.stdout)
            self.assertNotIn(str(elf), completed.stdout)
            self.assertNotIn(str(root), completed.stderr)
            return completed, document

    def test_compatible_uclibc_hard_float_binary(self):
        completed, document = self.run_verifier(
            "compatible", max_glibcxx="GLIBCXX_3.4.9"
        )

        self.assertEqual(completed.returncode, 0)
        self.assertTrue(document["compatible"])
        self.assertEqual(document["failed_checks"], [])
        self.assertEqual(
            document["checks"]["interpreter"]["observed"],
            "/lib/ld-uClibc.so.0",
        )

    def test_abi_mismatch_and_glibc_are_incompatible(self):
        completed, document = self.run_verifier("abi_mismatch")

        self.assertEqual(completed.returncode, 1)
        self.assertFalse(document["compatible"])
        self.assertIn("elf_class", document["failed_checks"])
        self.assertIn("machine", document["failed_checks"])
        self.assertIn("eabi", document["failed_checks"])
        self.assertIn("float_abi", document["failed_checks"])
        self.assertIn("interpreter", document["failed_checks"])
        self.assertIn("glibc_libc_so_6", document["failed_checks"])
        self.assertEqual(
            document["checks"]["interpreter"]["observed"], "unexpected"
        )

    def test_big_endian_arm_binary_is_incompatible(self):
        completed, document = self.run_verifier("big_endian")

        self.assertEqual(completed.returncode, 1)
        self.assertFalse(document["compatible"])
        self.assertEqual(document["failed_checks"], ["endianness"])
        self.assertEqual(document["checks"]["endianness"]["observed"], "big")

    def test_runpath_and_absolute_development_path_are_rejected(self):
        completed, document = self.run_verifier("rpath")

        self.assertEqual(completed.returncode, 1)
        self.assertTrue(document["checks"]["rpath_runpath"]["present"])
        self.assertTrue(
            document["checks"]["absolute_development_path"]["present"]
        )
        self.assertNotIn("private/build/output", completed.stdout)

    def test_glibcxx_newer_than_configured_ceiling_is_rejected(self):
        completed, document = self.run_verifier(
            "glibcxx_newer", max_glibcxx="3.4.9"
        )

        self.assertEqual(completed.returncode, 1)
        check = document["checks"]["max_glibcxx"]
        self.assertEqual(check["limit"], "GLIBCXX_3.4.9")
        self.assertEqual(check["observed"], "GLIBCXX_3.4.30")
        self.assertFalse(check["ok"])

    def test_readelf_error_is_path_free_tool_error(self):
        completed, document = self.run_verifier("tool_error")

        self.assertEqual(completed.returncode, 2)
        self.assertFalse(document["compatible"])
        self.assertEqual(document["error"]["code"], "readelf_failed")
        self.assertEqual(document["error"]["stage"], "elf_header")

    def test_missing_glibcxx_evidence_is_rejected_when_libstdcxx_is_needed(self):
        completed, document = self.run_verifier(
            "no_versions", max_glibcxx="GLIBCXX_3.4.25"
        )

        self.assertEqual(completed.returncode, 1)
        check = document["checks"]["max_glibcxx"]
        self.assertTrue(check["libstdcxx_present"])
        self.assertIsNone(check["observed"])
        self.assertFalse(check["ok"])


if __name__ == "__main__":
    unittest.main()
