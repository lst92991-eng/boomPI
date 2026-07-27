#!/usr/bin/env python3
"""Offline configure-time tests for the optional audio vendor CMake gate."""

from __future__ import annotations

import hashlib
from os import environ
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
VENDOR_MODULE = (
    REPOSITORY_ROOT / "client" / "cmake" / "BoompiAudioVendorDependencies.cmake"
)
ROCKCHIP_3A_LINK_CHECK_SOURCE = (
    REPOSITORY_ROOT / "client" / "tests" / "link" / "rockchip_3a_link_check.cpp"
)
PRIVATE_PATH_MARKER = "private-vendor-input"
FIXTURE_BYTES = b"boompi-audio-vendor-cmake-fixture\n"
FIXTURE_SHA256 = hashlib.sha256(FIXTURE_BYTES).hexdigest()

ROCKCHIP_PIN_VARIABLES = {
    "_BOOMPI_ROCKCHIP_3A_HEADER_SHA256": "header",
    "_BOOMPI_ROCKCHIP_3A_AEC_SHA256": "aec",
    "_BOOMPI_ROCKCHIP_3A_COMMON_SHA256": "common",
    "_BOOMPI_ROCKCHIP_3A_DETECT_SHA256": "detect",
    "_BOOMPI_ROCKCHIP_3A_CONFIG_SHA256": "config",
}


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
        if not ROCKCHIP_3A_LINK_CHECK_SOURCE.is_file():
            raise AssertionError(
                "Rockchip 3A link-check source is missing: "
                f"{ROCKCHIP_3A_LINK_CHECK_SOURCE}"
            )

    def _write_fixture_project(self, root: Path, *, languages: str = "NONE") -> Path:
        source = root / "source"
        source.mkdir()
        fixture = """\
cmake_minimum_required(VERSION 3.21)
project(boompi_audio_vendor_gate_fixture LANGUAGES @LANGUAGES@)

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

if(TEST_BYPASS_AUDIO_VENDOR_PLATFORM_GATES)
  # The normal gate behavior has separate tests above. This isolated fixture
  # exercises target construction with host-built stand-in shared libraries.
  function(_boompi_audio_vendor_require_rv1106_environment)
  endfunction()
  function(_boompi_audio_vendor_require_feasibility_mode)
  endfunction()
endif()

boompi_configure_audio_vendor_dependencies()

if(TEST_EXPECT_NO_ROCKCHIP_LINK_CHECK AND
   TARGET boompi_rockchip_3a_link_check)
  message(FATAL_ERROR "Rockchip link check must not exist while disabled")
endif()
if(TEST_EXPECT_ROCKCHIP_LINK_CHECK AND
   NOT TARGET boompi_rockchip_3a_link_check)
  message(FATAL_ERROR "Rockchip link check target was not created")
endif()
"""
        (source / "CMakeLists.txt").write_text(
            fixture.replace("@LANGUAGES@", languages),
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

    def _prepare_synthetic_rockchip_vendor(self, root: Path) -> dict[str, Path]:
        source = root / "synthetic-vendor-source"
        include_dir = source / "include"
        include_dir.mkdir(parents=True)
        (include_dir / "rkaudio_preprocess.h").write_text(
            """\
#pragma once

struct RKAUDIOParam {
  int reserved;
};

#ifdef __cplusplus
extern "C" {
#endif
void* rkaudio_preprocess_init(
    int rate, int bits, int src_chan, int ref_chan, RKAUDIOParam* param);
int rkaudio_preprocess_short(
    void* handle, short* input, short* output, int input_size,
    int* wakeup_status);
void rkaudio_preprocess_destory(void* handle);
#ifdef __cplusplus
}
#endif
""",
            encoding="utf-8",
        )
        (source / "common.cpp").write_text(
            'extern "C" int rkaudio_common_anchor() { return 7; }\n',
            encoding="utf-8",
        )
        (source / "detect.cpp").write_text(
            'extern "C" int rkaudio_detect_anchor() { return 11; }\n',
            encoding="utf-8",
        )
        (source / "aec.cpp").write_text(
            """\
#include "rkaudio_preprocess.h"

extern "C" int rkaudio_common_anchor();

#if !defined(BOOMPI_OMIT_INIT)
extern "C" void* rkaudio_preprocess_init(
    int, int, int, int, RKAUDIOParam* param) {
  (void)rkaudio_common_anchor();
  return param;
}
#endif

#if !defined(BOOMPI_OMIT_PROCESS)
extern "C" int rkaudio_preprocess_short(
    void*, short*, short*, int input_size, int*) {
  return input_size + rkaudio_common_anchor();
}
#endif

#if !defined(BOOMPI_OMIT_DESTROY)
extern "C" void rkaudio_preprocess_destory(void*) {
  (void)rkaudio_common_anchor();
}
#endif
""",
            encoding="utf-8",
        )
        (source / "CMakeLists.txt").write_text(
            """\
cmake_minimum_required(VERSION 3.21)
project(boompi_synthetic_rockchip_vendor LANGUAGES CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
foreach(_config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set("CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_config}"
      "${CMAKE_BINARY_DIR}/out")
endforeach()

add_library(rockchip_common SHARED common.cpp)
set_target_properties(rockchip_common PROPERTIES
  OUTPUT_NAME rkaudio_common PREFIX "lib" SUFFIX ".so"
  SKIP_BUILD_RPATH TRUE)

add_library(rockchip_detect SHARED detect.cpp)
set_target_properties(rockchip_detect PROPERTIES
  OUTPUT_NAME rkaudio_detect PREFIX "lib" SUFFIX ".so"
  SKIP_BUILD_RPATH TRUE)

function(add_synthetic_aec target_name output_name)
  add_library(${target_name} SHARED aec.cpp)
  target_include_directories(${target_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(${target_name} PRIVATE rockchip_common)
  set_target_properties(${target_name} PROPERTIES
    OUTPUT_NAME "${output_name}" PREFIX "lib" SUFFIX ".so"
    SKIP_BUILD_RPATH TRUE)
endfunction()

add_synthetic_aec(rockchip_aec_full aec_bf_process_full)
add_synthetic_aec(rockchip_aec_missing_init aec_bf_process_missing_init)
target_compile_definitions(rockchip_aec_missing_init PRIVATE BOOMPI_OMIT_INIT=1)
add_synthetic_aec(rockchip_aec_missing_process aec_bf_process_missing_process)
target_compile_definitions(rockchip_aec_missing_process PRIVATE BOOMPI_OMIT_PROCESS=1)
add_synthetic_aec(rockchip_aec_missing_destroy aec_bf_process_missing_destroy)
target_compile_definitions(rockchip_aec_missing_destroy PRIVATE BOOMPI_OMIT_DESTROY=1)
""",
            encoding="utf-8",
        )

        build = root / "synthetic-vendor-build"
        configured = subprocess.run(
            [
                str(self.cmake),
                "-S",
                str(source),
                "-B",
                str(build),
                "-DCMAKE_BUILD_TYPE:STRING=Release",
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(
            configured.returncode, 0, configured.stdout + configured.stderr
        )
        built = subprocess.run(
            [str(self.cmake), "--build", str(build), "--config", "Release"],
            check=False,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)

        output = build / "out"
        expected_outputs = {
            "common": output / "librkaudio_common.so",
            "detect": output / "librkaudio_detect.so",
            "aec_full": output / "libaec_bf_process_full.so",
            "aec_missing_init": output / "libaec_bf_process_missing_init.so",
            "aec_missing_process": output / "libaec_bf_process_missing_process.so",
            "aec_missing_destroy": output / "libaec_bf_process_missing_destroy.so",
        }
        for logical_name, artifact in expected_outputs.items():
            self.assertTrue(artifact.is_file(), f"missing {logical_name}: {artifact}")

        private_root = root / PRIVATE_PATH_MARKER
        private_include = private_root / "include"
        private_include.mkdir(parents=True)
        header = private_include / "rkaudio_preprocess.h"
        shutil.copy2(include_dir / "rkaudio_preprocess.h", header)
        config = private_root / "config_aivqe.json"
        config.write_text("{}\n", encoding="utf-8")

        artifacts: dict[str, Path] = {"header": header, "config": config}
        for logical_name in ("common", "detect"):
            destination = private_root / expected_outputs[logical_name].name
            shutil.copy2(expected_outputs[logical_name], destination)
            artifacts[logical_name] = destination
        for variant in (
            "aec_full",
            "aec_missing_init",
            "aec_missing_process",
            "aec_missing_destroy",
        ):
            variant_dir = private_root / variant
            variant_dir.mkdir()
            destination = variant_dir / "libaec_bf_process.so"
            shutil.copy2(expected_outputs[variant], destination)
            artifacts[variant] = destination
        return artifacts

    def _write_module_with_synthetic_rockchip_pins(
        self,
        root: Path,
        artifacts: dict[str, Path],
        aec_variant: str,
        case_name: str,
    ) -> Path:
        pinned_inputs = {
            "header": artifacts["header"],
            "aec": artifacts[aec_variant],
            "common": artifacts["common"],
            "detect": artifacts["detect"],
            "config": artifacts["config"],
        }
        module_root = root / f"synthetic-module-{case_name}" / "client"
        module_dir = module_root / "cmake"
        link_dir = module_root / "tests" / "link"
        module_dir.mkdir(parents=True)
        link_dir.mkdir(parents=True)

        module_text = VENDOR_MODULE.read_text(encoding="utf-8")
        for variable, logical_name in ROCKCHIP_PIN_VARIABLES.items():
            digest = hashlib.sha256(pinned_inputs[logical_name].read_bytes()).hexdigest()
            pattern = re.compile(
                rf'(set\({re.escape(variable)}\s+")[0-9A-Fa-f]{{64}}("\))'
            )
            module_text, replacements = pattern.subn(
                lambda match, value=digest: f"{match.group(1)}{value}{match.group(2)}",
                module_text,
            )
            self.assertEqual(replacements, 1, f"pin not found: {variable}")

        module = module_dir / VENDOR_MODULE.name
        module.write_text(module_text, encoding="utf-8")
        shutil.copy2(
            ROCKCHIP_3A_LINK_CHECK_SOURCE,
            link_dir / ROCKCHIP_3A_LINK_CHECK_SOURCE.name,
        )
        return module

    def _synthetic_rockchip_definitions(
        self,
        module: Path,
        artifacts: dict[str, Path],
        aec_variant: str,
    ) -> list[str]:
        return [
            f"-DBOOMPI_AUDIO_VENDOR_MODULE:FILEPATH={module}",
            "-DBOOMPI_ENABLE_ROCKCHIP_3A=ON",
            "-DBOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON",
            "-DBOOMPI_BUILD_TESTS=OFF",
            "-DCMAKE_BUILD_TYPE:STRING=Debug",
            "-DTEST_BYPASS_AUDIO_VENDOR_PLATFORM_GATES=ON",
            "-DTEST_EXPECT_ROCKCHIP_LINK_CHECK=ON",
            f"-DBOOMPI_ROCKCHIP_3A_INCLUDE_DIR:PATH={artifacts['header'].parent}",
            f"-DBOOMPI_ROCKCHIP_3A_AEC_LIBRARY:FILEPATH={artifacts[aec_variant]}",
            f"-DBOOMPI_ROCKCHIP_3A_COMMON_LIBRARY:FILEPATH={artifacts['common']}",
            f"-DBOOMPI_ROCKCHIP_3A_DETECT_LIBRARY:FILEPATH={artifacts['detect']}",
            f"-DBOOMPI_ROCKCHIP_3A_CONFIG_FILE:FILEPATH={artifacts['config']}",
        ]

    def _build(
        self, root: Path, case_name: str
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                str(self.cmake),
                "--build",
                str(root / f"build-{case_name}"),
                "--config",
                "Debug",
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=60,
        )

    def _find_link_check_executable(self, build: Path) -> Path:
        names = {"boompi_rockchip_3a_link_check", "boompi_rockchip_3a_link_check.exe"}
        candidates = [
            candidate
            for candidate in build.rglob("boompi_rockchip_3a_link_check*")
            if candidate.is_file() and candidate.name in names
        ]
        self.assertEqual(
            len(candidates),
            1,
            "Rockchip 3A link-check executable was not built exactly once by default ALL",
        )
        return candidates[0]

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
                "-DTEST_EXPECT_NO_ROCKCHIP_LINK_CHECK=ON",
            )

        output = completed.stdout + completed.stderr
        self.assertEqual(completed.returncode, 0, output)
        self.assertNotIn(PRIVATE_PATH_MARKER, output)

    @unittest.skipUnless(
        sys.platform.startswith("linux"),
        "the enabled Rockchip fixture exercises Linux shared-object linking",
    )
    def test_rockchip_link_check_is_default_all_with_tests_off(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-audio-vendor-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root, languages="CXX")
            artifacts = self._prepare_synthetic_rockchip_vendor(root)
            module = self._write_module_with_synthetic_rockchip_pins(
                root, artifacts, "aec_full", "full"
            )

            configured = self._configure(
                root,
                "rockchip-link-full",
                *self._synthetic_rockchip_definitions(
                    module, artifacts, "aec_full"
                ),
            )
            self.assertEqual(
                configured.returncode, 0, configured.stdout + configured.stderr
            )

            built = self._build(root, "rockchip-link-full")
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            executable = self._find_link_check_executable(
                root / "build-rockchip-link-full"
            )

            readelf = shutil.which("readelf")
            self.assertIsNotNone(
                readelf,
                "readelf is required to verify that the link-check has no RPATH",
            )
            dynamic = subprocess.run(
                [readelf, "-d", str(executable)],
                check=False,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(dynamic.returncode, 0, dynamic.stdout + dynamic.stderr)
            self.assertNotRegex(dynamic.stdout, r"\((?:RPATH|RUNPATH)\)")
            self.assertNotIn(PRIVATE_PATH_MARKER, dynamic.stdout)

    @unittest.skipUnless(
        sys.platform.startswith("linux"),
        "the enabled Rockchip fixture exercises Linux shared-object linking",
    )
    def test_rockchip_link_check_requires_each_direct_symbol(self) -> None:
        missing_variants = (
            ("aec_missing_init", "rkaudio_preprocess_init"),
            ("aec_missing_process", "rkaudio_preprocess_short"),
            ("aec_missing_destroy", "rkaudio_preprocess_destory"),
        )
        with tempfile.TemporaryDirectory(prefix="boompi-audio-vendor-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root, languages="CXX")
            artifacts = self._prepare_synthetic_rockchip_vendor(root)

            for variant, symbol in missing_variants:
                with self.subTest(symbol=symbol):
                    module = self._write_module_with_synthetic_rockchip_pins(
                        root, artifacts, variant, variant
                    )
                    case_name = f"rockchip-link-{variant}"
                    configured = self._configure(
                        root,
                        case_name,
                        *self._synthetic_rockchip_definitions(
                            module, artifacts, variant
                        ),
                    )
                    self.assertEqual(
                        configured.returncode,
                        0,
                        configured.stdout + configured.stderr,
                    )

                    built = self._build(root, case_name)
                    output = built.stdout + built.stderr
                    self.assertNotEqual(built.returncode, 0, output)
                    self.assertIn(symbol, output)

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
