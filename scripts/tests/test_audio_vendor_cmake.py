#!/usr/bin/env python3
"""Offline configure-time tests for the optional audio vendor CMake gate."""

from __future__ import annotations

import hashlib
from os import environ
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
VENDOR_MODULE = (
    REPOSITORY_ROOT / "client" / "cmake" / "BoompiAudioVendorDependencies.cmake"
)
PRIVATE_PATH_MARKER = "private-vendor-input"
FIXTURE_BYTES = b"boompi-audio-vendor-cmake-fixture\n"
FIXTURE_SHA256 = hashlib.sha256(FIXTURE_BYTES).hexdigest()


def _find_cmake() -> Path | None:
    configured = environ.get("CMAKE")
    candidates = [
        Path(configured) if configured else None,
        Path(found) if (found := shutil.which("cmake")) else None,
    ]
    return next((candidate for candidate in candidates if candidate and candidate.is_file()), None)


class AudioVendorCMakeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cmake = _find_cmake()
        if cls.cmake is None:
            raise unittest.SkipTest("CMake executable was not found")
        if not VENDOR_MODULE.is_file():
            raise AssertionError(f"audio vendor CMake module is missing: {VENDOR_MODULE}")

    def _write_fixture_project(self, root: Path) -> Path:
        source = root / "source"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(
            """\
cmake_minimum_required(VERSION 3.21)
project(boompi_audio_vendor_gate_fixture LANGUAGES NONE)

if(NOT DEFINED BOOMPI_AUDIO_VENDOR_MODULE)
  message(FATAL_ERROR "BOOMPI_AUDIO_VENDOR_MODULE is required")
endif()

include("${BOOMPI_AUDIO_VENDOR_MODULE}")

if(TEST_TARGET_GATE)
  _boompi_audio_vendor_require_rv1106_environment()
endif()
if(TEST_FEASIBILITY_GATE)
  _boompi_audio_vendor_require_feasibility_mode()
endif()

if(DEFINED TEST_HELPER_PATH)
  boompi_audio_vendor_require_pinned_file(
    "TEST_VENDOR_FILE"
    "${TEST_HELPER_PATH}"
    "${TEST_HELPER_SHA256}"
    _validated_fixture
  )
  if(NOT IS_ABSOLUTE "${_validated_fixture}")
    message(FATAL_ERROR "validated helper output must be absolute")
  endif()
endif()

boompi_configure_audio_vendor_dependencies()
""",
            encoding="utf-8",
        )
        return source

    def _write_synthetic_target_environment(self, root: Path) -> list[str]:
        compiler = root / "toolchain" / "arm-rockchip830-linux-uclibcgnueabihf-g++"
        compiler.parent.mkdir()
        compiler.write_bytes(b"synthetic compiler identity; never executed\n")
        sysroot = root / "sysroot"
        loader = sysroot / "lib" / "ld-uClibc.so.0"
        loader.parent.mkdir(parents=True)
        loader.write_bytes(b"synthetic loader identity; never executed\n")
        return [
            "-DBOOMPI_TARGET_RV1106=ON",
            "-DCMAKE_SYSTEM_NAME:STRING=Linux",
            "-DCMAKE_SYSTEM_PROCESSOR:STRING=arm",
            "-DCMAKE_CROSSCOMPILING:BOOL=ON",
            f"-DCMAKE_CXX_COMPILER:FILEPATH={compiler}",
            "-DCMAKE_CXX_COMPILER_ID:STRING=GNU",
            f"-DCMAKE_SYSROOT:PATH={sysroot}",
            "-DCMAKE_BUILD_TYPE:STRING=Debug",
            "-DCMAKE_CONFIGURATION_TYPES:STRING=Debug",
        ]

    def _configure(
        self, root: Path, case_name: str, *definitions: str
    ) -> subprocess.CompletedProcess[str]:
        source = root / "source"
        build = root / f"build-{case_name}"
        command = [
            str(self.cmake),
            "-S",
            str(source),
            "-B",
            str(build),
            f"-DBOOMPI_AUDIO_VENDOR_MODULE:FILEPATH={VENDOR_MODULE}",
            *definitions,
        ]
        return subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
        )

    def _assert_failed_without_private_path(
        self, completed: subprocess.CompletedProcess[str]
    ) -> None:
        output = completed.stdout + completed.stderr
        self.assertNotEqual(completed.returncode, 0, output)
        self.assertNotIn(PRIVATE_PATH_MARKER, output)

    def test_default_off_configures_without_vendor_inputs(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-audio-vendor-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root)
            private_root = root / PRIVATE_PATH_MARKER

            completed = self._configure(
                root,
                "default-off",
                f"-DBOOMPI_ROCKCHIP_3A_INCLUDE_DIR:PATH={private_root / 'rockchip-include'}",
                f"-DBOOMPI_ROCKCHIP_3A_AEC_LIBRARY:FILEPATH={private_root / 'missing-aec.so'}",
                f"-DBOOMPI_ROCKCHIP_3A_COMMON_LIBRARY:FILEPATH={private_root / 'missing-common.so'}",
                f"-DBOOMPI_ROCKCHIP_3A_DETECT_LIBRARY:FILEPATH={private_root / 'missing-detect.so'}",
                f"-DBOOMPI_ROCKCHIP_3A_CONFIG_FILE:FILEPATH={private_root / 'missing-config.json'}",
                f"-DBOOMPI_SNOWBOY_INCLUDE_DIR:PATH={private_root / 'snowboy-include'}",
                f"-DBOOMPI_SNOWBOY_LIBRARY:FILEPATH={private_root / 'missing-snowboy.a'}",
                f"-DBOOMPI_SNOWBOY_RESOURCE_FILE:FILEPATH={private_root / 'missing-resource.res'}",
                f"-DBOOMPI_SNOWBOY_MODEL_FILE:FILEPATH={private_root / 'missing-model.pmdl'}",
                f"-DBOOMPI_OPENBLAS_LIBRARY:FILEPATH={private_root / 'missing-openblas.a'}",
            )

        output = completed.stdout + completed.stderr
        self.assertEqual(completed.returncode, 0, output)
        self.assertNotIn(PRIVATE_PATH_MARKER, output)

    def test_host_enabling_either_vendor_is_rejected(self) -> None:
        vendor_options = (
            "BOOMPI_ENABLE_ROCKCHIP_3A",
            "BOOMPI_ENABLE_SNOWBOY",
        )
        for option in vendor_options:
            with self.subTest(option=option):
                with tempfile.TemporaryDirectory(
                    prefix="boompi-audio-vendor-cmake-"
                ) as temporary:
                    root = Path(temporary)
                    self._write_fixture_project(root)

                    completed = self._configure(root, option.lower(), f"-D{option}=ON")

                output = completed.stdout + completed.stderr
                self.assertNotEqual(completed.returncode, 0, output)
                self.assertIn("BOOMPI_TARGET_RV1106", output)

    def test_target_gate_rejects_host_boolean_spoof(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-audio-vendor-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root)

            completed = self._configure(
                root,
                "host-spoof",
                "-DBOOMPI_TARGET_RV1106=ON",
                "-DBOOMPI_ENABLE_SNOWBOY=ON",
            )

        output = completed.stdout + completed.stderr
        self.assertNotEqual(completed.returncode, 0, output)
        self.assertIn("cross-compiled Linux/ARM", output)

    def test_target_and_feasibility_gates_accept_only_debug_probe(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-audio-vendor-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root)
            target_definitions = self._write_synthetic_target_environment(root)

            target_only = self._configure(
                root,
                "target-only",
                *target_definitions,
                "-DTEST_TARGET_GATE=ON",
            )
            no_opt_in = self._configure(
                root,
                "feasibility-no-opt-in",
                *target_definitions,
                "-DTEST_TARGET_GATE=ON",
                "-DTEST_FEASIBILITY_GATE=ON",
            )
            release = self._configure(
                root,
                "feasibility-release",
                *target_definitions,
                "-DBOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON",
                "-DCMAKE_BUILD_TYPE:STRING=Release",
                "-DCMAKE_CONFIGURATION_TYPES:STRING=Release",
                "-DTEST_TARGET_GATE=ON",
                "-DTEST_FEASIBILITY_GATE=ON",
            )
            debug = self._configure(
                root,
                "feasibility-debug",
                *target_definitions,
                "-DBOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON",
                "-DTEST_TARGET_GATE=ON",
                "-DTEST_FEASIBILITY_GATE=ON",
            )

        self.assertEqual(
            target_only.returncode, 0, target_only.stdout + target_only.stderr
        )
        no_opt_in_output = no_opt_in.stdout + no_opt_in.stderr
        self.assertNotEqual(no_opt_in.returncode, 0, no_opt_in_output)
        self.assertIn("feasibility-only", no_opt_in_output)
        release_output = release.stdout + release.stderr
        self.assertNotEqual(release.returncode, 0, release_output)
        self.assertIn("Debug-only", release_output)
        self.assertEqual(debug.returncode, 0, debug.stdout + debug.stderr)

    def test_vendor_on_reaches_pinned_input_validation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-audio-vendor-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root)
            target_definitions = self._write_synthetic_target_environment(root)

            completed = self._configure(
                root,
                "vendor-on",
                *target_definitions,
                "-DBOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON",
                "-DBOOMPI_ENABLE_ROCKCHIP_3A=ON",
            )

        output = completed.stdout + completed.stderr
        self.assertNotEqual(completed.returncode, 0, output)
        self.assertIn("BOOMPI_ROCKCHIP_3A_INCLUDE_DIR", output)

    def test_sha_helper_accepts_correct_hash(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-audio-vendor-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root)
            fixture = root / "fixture.bin"
            fixture.write_bytes(FIXTURE_BYTES)

            completed = self._configure(
                root,
                "correct-hash",
                f"-DTEST_HELPER_PATH:FILEPATH={fixture}",
                f"-DTEST_HELPER_SHA256:STRING={FIXTURE_SHA256}",
            )

        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)

    def test_sha_helper_rejects_wrong_hash_without_path_leak(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-audio-vendor-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root)
            fixture = root / PRIVATE_PATH_MARKER / "fixture.bin"
            fixture.parent.mkdir()
            fixture.write_bytes(FIXTURE_BYTES)

            completed = self._configure(
                root,
                "wrong-hash",
                f"-DTEST_HELPER_PATH:FILEPATH={fixture}",
                f"-DTEST_HELPER_SHA256:STRING={'0' * 64}",
            )

        self._assert_failed_without_private_path(completed)

    def test_sha_helper_rejects_missing_file_without_path_leak(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-audio-vendor-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root)
            missing = root / PRIVATE_PATH_MARKER / "missing.bin"

            completed = self._configure(
                root,
                "missing-file",
                f"-DTEST_HELPER_PATH:FILEPATH={missing}",
                f"-DTEST_HELPER_SHA256:STRING={FIXTURE_SHA256}",
            )

        self._assert_failed_without_private_path(completed)

    def test_sha_helper_rejects_relative_path_without_path_leak(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-audio-vendor-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root)
            relative = f"{PRIVATE_PATH_MARKER}/fixture.bin"

            completed = self._configure(
                root,
                "relative-path",
                f"-DTEST_HELPER_PATH:STRING={relative}",
                f"-DTEST_HELPER_SHA256:STRING={FIXTURE_SHA256}",
            )

        self._assert_failed_without_private_path(completed)

    def test_sha_helper_rejects_directory_without_path_leak(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-audio-vendor-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root)
            directory = root / PRIVATE_PATH_MARKER
            directory.mkdir()

            completed = self._configure(
                root,
                "directory",
                f"-DTEST_HELPER_PATH:PATH={directory}",
                f"-DTEST_HELPER_SHA256:STRING={FIXTURE_SHA256}",
            )

        self._assert_failed_without_private_path(completed)


if __name__ == "__main__":
    unittest.main()
