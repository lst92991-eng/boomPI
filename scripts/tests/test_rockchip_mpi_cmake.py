#!/usr/bin/env python3
"""Host tests for the optional Rockchip MPI audio CMake link check."""

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
LINK_CHECK_SOURCE = (
    REPOSITORY_ROOT
    / "client"
    / "tests"
    / "link"
    / "rockchip_mpi_audio_link_check.cpp"
)
PRIVATE_PATH_MARKER = "private-mpi-vendor-input"

HEADER_PINS = {
    "_BOOMPI_ROCKCHIP_MPI_AI_HEADER_SHA256": "rk_mpi_ai.h",
    "_BOOMPI_ROCKCHIP_MPI_AO_HEADER_SHA256": "rk_mpi_ao.h",
    "_BOOMPI_ROCKCHIP_MPI_AIO_HEADER_SHA256": "rk_comm_aio.h",
    "_BOOMPI_ROCKCHIP_MPI_SYS_HEADER_SHA256": "rk_mpi_sys.h",
    "_BOOMPI_ROCKCHIP_MPI_MB_HEADER_SHA256": "rk_mpi_mb.h",
    "_BOOMPI_ROCKCHIP_MPI_COMM_MB_HEADER_SHA256": "rk_comm_mb.h",
    "_BOOMPI_ROCKCHIP_MPI_COMMON_HEADER_SHA256": "rk_common.h",
    "_BOOMPI_ROCKCHIP_MPI_TYPE_HEADER_SHA256": "rk_type.h",
}
LIBRARY_PINS = {
    "_BOOMPI_ROCKCHIP_MPI_ROCKIT_SHA256": "rockit",
    "_BOOMPI_ROCKCHIP_MPI_MPP_SHA256": "mpp",
    "_BOOMPI_ROCKCHIP_MPI_RGA_SHA256": "rga",
}


def _find_cmake() -> Path | None:
    configured = environ.get("CMAKE")
    candidates = [
        Path(configured) if configured else None,
        Path(found) if (found := shutil.which("cmake")) else None,
    ]
    return next((candidate for candidate in candidates if candidate and candidate.is_file()), None)


class RockchipMpiCMakeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cmake = _find_cmake()
        if cls.cmake is None:
            raise unittest.SkipTest("CMake executable was not found")
        if not VENDOR_MODULE.is_file():
            raise AssertionError(f"audio vendor CMake module is missing: {VENDOR_MODULE}")
        if not LINK_CHECK_SOURCE.is_file():
            raise AssertionError(f"MPI audio link-check source is missing: {LINK_CHECK_SOURCE}")

    def _write_fixture_project(self, root: Path, *, languages: str = "NONE") -> None:
        source = root / "source"
        source.mkdir()
        fixture = """\
cmake_minimum_required(VERSION 3.21)
project(boompi_rockchip_mpi_gate_fixture LANGUAGES @LANGUAGES@)

if(NOT DEFINED BOOMPI_AUDIO_VENDOR_MODULE)
  message(FATAL_ERROR "BOOMPI_AUDIO_VENDOR_MODULE is required")
endif()
include("${BOOMPI_AUDIO_VENDOR_MODULE}")

if(TEST_CONFIGURE_MPI_LINK_CHECK)
  # Platform and feasibility fail-closed behavior is tested through the public
  # entry point separately. This calls only the dependency constructor with
  # host-built stand-ins whose hashes are pinned in a temporary module copy.
  _boompi_configure_rockchip_mpi_audio_dependency()
else()
  boompi_configure_audio_vendor_dependencies()
endif()

if(TEST_EXPECT_NO_MPI_TARGET AND TARGET boompi_rockchip_mpi_audio_link_check)
  message(FATAL_ERROR "MPI link check must not exist while disabled")
endif()
if(TEST_EXPECT_MPI_TARGET AND NOT TARGET boompi_rockchip_mpi_audio_link_check)
  message(FATAL_ERROR "MPI link-check target was not created")
endif()
"""
        (source / "CMakeLists.txt").write_text(
            fixture.replace("@LANGUAGES@", languages), encoding="utf-8"
        )

    def _configure(
        self, root: Path, case_name: str, *definitions: str
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                str(self.cmake),
                "-S",
                str(root / "source"),
                "-B",
                str(root / f"build-{case_name}"),
                f"-DBOOMPI_AUDIO_VENDOR_MODULE:FILEPATH={VENDOR_MODULE}",
                *definitions,
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=60,
        )

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

    def _write_synthetic_headers(self, include_dir: Path) -> None:
        include_dir.mkdir(parents=True)
        headers = {
            "rk_type.h": """\
#pragma once
typedef int RK_S32;
typedef unsigned int RK_U32;
typedef unsigned long long RK_U64;
typedef void RK_VOID;
""",
            "rk_common.h": """\
#pragma once
#include "rk_type.h"
""",
            "rk_comm_mb.h": """\
#pragma once
#include "rk_common.h"
typedef RK_VOID* MB_BLK;
typedef struct rkMB_EXT_CONFIG_S { RK_U32 reserved; } MB_EXT_CONFIG_S;
""",
            "rk_comm_aio.h": """\
#pragma once
#include "rk_comm_mb.h"
typedef RK_S32 AUDIO_DEV;
typedef RK_S32 AI_CHN;
typedef RK_S32 AO_CHN;
typedef struct rkAIO_ATTR_S { RK_U32 reserved; } AIO_ATTR_S;
typedef struct rkAI_CHN_PARAM_S { RK_U32 reserved; } AI_CHN_PARAM_S;
typedef struct rkAO_CHN_PARAM_S { RK_U32 reserved; } AO_CHN_PARAM_S;
typedef struct rkAUDIO_FRAME_S { MB_BLK block; } AUDIO_FRAME_S;
typedef struct rkAEC_FRAME_S { AUDIO_FRAME_S frame; } AEC_FRAME_S;
""",
            "rk_mpi_sys.h": """\
#pragma once
#include "rk_comm_mb.h"
#ifdef __cplusplus
extern "C" {
#endif
RK_S32 RK_MPI_SYS_Init(RK_VOID);
RK_S32 RK_MPI_SYS_Exit(RK_VOID);
RK_S32 RK_MPI_SYS_CreateMB(MB_BLK* block, MB_EXT_CONFIG_S* config);
#ifdef __cplusplus
}
#endif
""",
            "rk_mpi_mb.h": """\
#pragma once
#include "rk_comm_mb.h"
#ifdef __cplusplus
extern "C" {
#endif
RK_VOID* RK_MPI_MB_Handle2VirAddr(MB_BLK block);
RK_S32 RK_MPI_MB_ReleaseMB(MB_BLK block);
#ifdef __cplusplus
}
#endif
""",
            "rk_mpi_ai.h": """\
#pragma once
#include "rk_comm_aio.h"
#ifdef __cplusplus
extern "C" {
#endif
RK_S32 RK_MPI_AI_SetPubAttr(AUDIO_DEV device, const AIO_ATTR_S* attributes);
RK_S32 RK_MPI_AI_Enable(AUDIO_DEV device);
RK_S32 RK_MPI_AI_EnableChn(AUDIO_DEV device, AI_CHN channel);
RK_S32 RK_MPI_AI_SetChnParam(
    AUDIO_DEV device, AI_CHN channel, const AI_CHN_PARAM_S* parameters);
RK_S32 RK_MPI_AI_GetFrame(
    AUDIO_DEV device, AI_CHN channel, AUDIO_FRAME_S* frame,
    AEC_FRAME_S* reference, RK_S32 timeout_ms);
RK_S32 RK_MPI_AI_ReleaseFrame(
    AUDIO_DEV device, AI_CHN channel, const AUDIO_FRAME_S* frame,
    const AEC_FRAME_S* reference);
RK_S32 RK_MPI_AI_DisableChn(AUDIO_DEV device, AI_CHN channel);
RK_S32 RK_MPI_AI_Disable(AUDIO_DEV device);
#ifdef __cplusplus
}
#endif
""",
            "rk_mpi_ao.h": """\
#pragma once
#include "rk_comm_aio.h"
#ifdef __cplusplus
extern "C" {
#endif
RK_S32 RK_MPI_AO_SetPubAttr(AUDIO_DEV device, const AIO_ATTR_S* attributes);
RK_S32 RK_MPI_AO_Enable(AUDIO_DEV device);
RK_S32 RK_MPI_AO_EnableChn(AUDIO_DEV device, AO_CHN channel);
RK_S32 RK_MPI_AO_SetChnParams(
    AUDIO_DEV device, AO_CHN channel, const AO_CHN_PARAM_S* parameters);
RK_S32 RK_MPI_AO_SendFrame(
    AUDIO_DEV device, AO_CHN channel, const AUDIO_FRAME_S* frame,
    RK_S32 timeout_ms);
RK_S32 RK_MPI_AO_WaitEos(AUDIO_DEV device, AO_CHN channel, RK_S32 timeout_ms);
RK_S32 RK_MPI_AO_DisableChn(AUDIO_DEV device, AO_CHN channel);
RK_S32 RK_MPI_AO_Disable(AUDIO_DEV device);
#ifdef __cplusplus
}
#endif
""",
        }
        for name, contents in headers.items():
            (include_dir / name).write_text(contents, encoding="utf-8")

    def _prepare_synthetic_vendor(self, root: Path) -> dict[str, Path]:
        source = root / "synthetic-mpi-vendor-source"
        include_dir = source / "include"
        self._write_synthetic_headers(include_dir)
        (source / "mpp.cpp").write_text(
            'extern "C" int boompi_mpp_anchor() { return 3; }\n', encoding="utf-8"
        )
        (source / "rga.cpp").write_text(
            'extern "C" int boompi_rga_anchor() { return 5; }\n', encoding="utf-8"
        )
        (source / "rockit.cpp").write_text(
            """\
#include "rk_mpi_ai.h"
#include "rk_mpi_ao.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"

extern "C" int boompi_mpp_anchor();
extern "C" int boompi_rga_anchor();

#if !defined(BOOMPI_OMIT_SYS)
extern "C" RK_S32 RK_MPI_SYS_Init(RK_VOID) {
  return boompi_mpp_anchor() + boompi_rga_anchor();
}
#endif
extern "C" RK_S32 RK_MPI_SYS_Exit(RK_VOID) { return 0; }
extern "C" RK_S32 RK_MPI_SYS_CreateMB(MB_BLK*, MB_EXT_CONFIG_S*) { return 0; }
#if !defined(BOOMPI_OMIT_MB)
extern "C" RK_VOID* RK_MPI_MB_Handle2VirAddr(MB_BLK block) { return block; }
#endif
extern "C" RK_S32 RK_MPI_MB_ReleaseMB(MB_BLK) { return 0; }

extern "C" RK_S32 RK_MPI_AI_SetPubAttr(AUDIO_DEV, const AIO_ATTR_S*) { return 0; }
extern "C" RK_S32 RK_MPI_AI_Enable(AUDIO_DEV) { return 0; }
extern "C" RK_S32 RK_MPI_AI_EnableChn(AUDIO_DEV, AI_CHN) { return 0; }
extern "C" RK_S32 RK_MPI_AI_SetChnParam(
    AUDIO_DEV, AI_CHN, const AI_CHN_PARAM_S*) { return 0; }
#if !defined(BOOMPI_OMIT_AI)
extern "C" RK_S32 RK_MPI_AI_GetFrame(
    AUDIO_DEV, AI_CHN, AUDIO_FRAME_S*, AEC_FRAME_S*, RK_S32) { return 0; }
#endif
extern "C" RK_S32 RK_MPI_AI_ReleaseFrame(
    AUDIO_DEV, AI_CHN, const AUDIO_FRAME_S*, const AEC_FRAME_S*) { return 0; }
extern "C" RK_S32 RK_MPI_AI_DisableChn(AUDIO_DEV, AI_CHN) { return 0; }
extern "C" RK_S32 RK_MPI_AI_Disable(AUDIO_DEV) { return 0; }

extern "C" RK_S32 RK_MPI_AO_SetPubAttr(AUDIO_DEV, const AIO_ATTR_S*) { return 0; }
extern "C" RK_S32 RK_MPI_AO_Enable(AUDIO_DEV) { return 0; }
extern "C" RK_S32 RK_MPI_AO_EnableChn(AUDIO_DEV, AO_CHN) { return 0; }
extern "C" RK_S32 RK_MPI_AO_SetChnParams(
    AUDIO_DEV, AO_CHN, const AO_CHN_PARAM_S*) { return 0; }
#if !defined(BOOMPI_OMIT_AO)
extern "C" RK_S32 RK_MPI_AO_SendFrame(
    AUDIO_DEV, AO_CHN, const AUDIO_FRAME_S*, RK_S32) { return 0; }
#endif
extern "C" RK_S32 RK_MPI_AO_WaitEos(AUDIO_DEV, AO_CHN, RK_S32) { return 0; }
extern "C" RK_S32 RK_MPI_AO_DisableChn(AUDIO_DEV, AO_CHN) { return 0; }
extern "C" RK_S32 RK_MPI_AO_Disable(AUDIO_DEV) { return 0; }
""",
            encoding="utf-8",
        )
        (source / "CMakeLists.txt").write_text(
            """\
cmake_minimum_required(VERSION 3.21)
project(boompi_synthetic_mpi_vendor LANGUAGES CXX)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
foreach(_config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set("CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_config}" "${CMAKE_BINARY_DIR}/out")
endforeach()

add_library(mpp SHARED mpp.cpp)
set_target_properties(mpp PROPERTIES OUTPUT_NAME rockchip_mpp PREFIX "lib"
  SUFFIX ".so" SOVERSION 1 SKIP_BUILD_RPATH TRUE)
add_library(rga SHARED rga.cpp)
set_target_properties(rga PROPERTIES OUTPUT_NAME rga PREFIX "lib" SUFFIX ".so"
  SKIP_BUILD_RPATH TRUE)

function(add_synthetic_rockit target_name output_name)
  add_library(${target_name} SHARED rockit.cpp)
  target_include_directories(${target_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(${target_name} PRIVATE mpp rga)
  set_target_properties(${target_name} PROPERTIES OUTPUT_NAME "${output_name}"
    PREFIX "lib" SUFFIX ".so" SKIP_BUILD_RPATH TRUE)
endfunction()
add_synthetic_rockit(rockit_full rockit_full)
add_synthetic_rockit(rockit_missing_sys rockit_missing_sys)
target_compile_definitions(rockit_missing_sys PRIVATE BOOMPI_OMIT_SYS=1)
add_synthetic_rockit(rockit_missing_mb rockit_missing_mb)
target_compile_definitions(rockit_missing_mb PRIVATE BOOMPI_OMIT_MB=1)
add_synthetic_rockit(rockit_missing_ai rockit_missing_ai)
target_compile_definitions(rockit_missing_ai PRIVATE BOOMPI_OMIT_AI=1)
add_synthetic_rockit(rockit_missing_ao rockit_missing_ao)
target_compile_definitions(rockit_missing_ao PRIVATE BOOMPI_OMIT_AO=1)
""",
            encoding="utf-8",
        )

        build = root / "synthetic-mpi-vendor-build"
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
        self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
        built = subprocess.run(
            [str(self.cmake), "--build", str(build), "--config", "Release"],
            check=False,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)

        output = build / "out"
        built_artifacts = {
            "mpp": output / "librockchip_mpp.so.1",
            "rga": output / "librga.so",
            "rockit_full": output / "librockit_full.so",
            "rockit_missing_sys": output / "librockit_missing_sys.so",
            "rockit_missing_mb": output / "librockit_missing_mb.so",
            "rockit_missing_ai": output / "librockit_missing_ai.so",
            "rockit_missing_ao": output / "librockit_missing_ao.so",
        }
        for logical_name, artifact in built_artifacts.items():
            self.assertTrue(artifact.is_file(), f"missing {logical_name}: {artifact}")

        private_root = root / PRIVATE_PATH_MARKER
        private_include = private_root / "include"
        shutil.copytree(include_dir, private_include)
        artifacts: dict[str, Path] = {"include": private_include}
        for logical_name in ("mpp", "rga"):
            destination = private_root / built_artifacts[logical_name].name
            shutil.copy2(built_artifacts[logical_name], destination)
            artifacts[logical_name] = destination
        for variant in (
            "rockit_full",
            "rockit_missing_sys",
            "rockit_missing_mb",
            "rockit_missing_ai",
            "rockit_missing_ao",
        ):
            variant_dir = private_root / variant
            variant_dir.mkdir()
            destination = variant_dir / "librockit.so"
            shutil.copy2(built_artifacts[variant], destination)
            artifacts[variant] = destination
        return artifacts

    def _write_pinned_module(
        self,
        root: Path,
        artifacts: dict[str, Path],
        rockit_variant: str,
        case_name: str,
    ) -> Path:
        module_root = root / f"synthetic-mpi-module-{case_name}" / "client"
        module_dir = module_root / "cmake"
        link_dir = module_root / "tests" / "link"
        module_dir.mkdir(parents=True)
        link_dir.mkdir(parents=True)

        pinned_inputs = {
            **{
                header_name: artifacts["include"] / header_name
                for header_name in HEADER_PINS.values()
            },
            "rockit": artifacts[rockit_variant],
            "mpp": artifacts["mpp"],
            "rga": artifacts["rga"],
        }
        module_text = VENDOR_MODULE.read_text(encoding="utf-8")
        for variable, logical_name in {**HEADER_PINS, **LIBRARY_PINS}.items():
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
        shutil.copy2(LINK_CHECK_SOURCE, link_dir / LINK_CHECK_SOURCE.name)
        return module

    def _enabled_definitions(
        self,
        module: Path,
        artifacts: dict[str, Path],
        rockit_variant: str,
    ) -> list[str]:
        return [
            f"-DBOOMPI_AUDIO_VENDOR_MODULE:FILEPATH={module}",
            "-DBOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO=ON",
            "-DBOOMPI_BUILD_TESTS=OFF",
            "-DCMAKE_BUILD_TYPE:STRING=Debug",
            "-DTEST_CONFIGURE_MPI_LINK_CHECK=ON",
            "-DTEST_EXPECT_MPI_TARGET=ON",
            f"-DBOOMPI_ROCKCHIP_MPI_INCLUDE_DIR:PATH={artifacts['include']}",
            f"-DBOOMPI_ROCKCHIP_MPI_ROCKIT_LIBRARY:FILEPATH={artifacts[rockit_variant]}",
            f"-DBOOMPI_ROCKCHIP_MPI_MPP_LIBRARY:FILEPATH={artifacts['mpp']}",
            f"-DBOOMPI_ROCKCHIP_MPI_RGA_LIBRARY:FILEPATH={artifacts['rga']}",
        ]

    def _find_link_check(self, build: Path) -> Path:
        names = {
            "boompi_rockchip_mpi_audio_link_check",
            "boompi_rockchip_mpi_audio_link_check.exe",
        }
        candidates = [
            candidate
            for candidate in build.rglob("boompi_rockchip_mpi_audio_link_check*")
            if candidate.is_file() and candidate.name in names
        ]
        self.assertEqual(
            len(candidates), 1, "MPI link-check executable was not built once by ALL"
        )
        return candidates[0]

    def test_default_off_does_not_access_paths_or_create_target(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-rockchip-mpi-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root)
            private = root / PRIVATE_PATH_MARKER
            configured = self._configure(
                root,
                "default-off",
                f"-DBOOMPI_ROCKCHIP_MPI_INCLUDE_DIR:PATH={private / 'include'}",
                f"-DBOOMPI_ROCKCHIP_MPI_ROCKIT_LIBRARY:FILEPATH={private / 'librockit.so'}",
                f"-DBOOMPI_ROCKCHIP_MPI_MPP_LIBRARY:FILEPATH={private / 'libmpp.so'}",
                f"-DBOOMPI_ROCKCHIP_MPI_RGA_LIBRARY:FILEPATH={private / 'librga.so'}",
                "-DTEST_EXPECT_NO_MPI_TARGET=ON",
            )
        output = configured.stdout + configured.stderr
        self.assertEqual(configured.returncode, 0, output)
        self.assertNotIn(PRIVATE_PATH_MARKER, output)

    def test_host_enable_fails_closed_before_vendor_paths(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-rockchip-mpi-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root)
            private = root / PRIVATE_PATH_MARKER
            configured = self._configure(
                root,
                "host-enable",
                "-DBOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO=ON",
                f"-DBOOMPI_ROCKCHIP_MPI_INCLUDE_DIR:PATH={private / 'include'}",
                f"-DBOOMPI_ROCKCHIP_MPI_ROCKIT_LIBRARY:FILEPATH={private / 'librockit.so'}",
                f"-DBOOMPI_ROCKCHIP_MPI_MPP_LIBRARY:FILEPATH={private / 'libmpp.so'}",
                f"-DBOOMPI_ROCKCHIP_MPI_RGA_LIBRARY:FILEPATH={private / 'librga.so'}",
            )
        output = configured.stdout + configured.stderr
        self.assertNotEqual(configured.returncode, 0, output)
        self.assertIn("BOOMPI_TARGET_RV1106", output)
        self.assertNotIn(PRIVATE_PATH_MARKER, output)

    @unittest.skipUnless(
        sys.platform.startswith("linux"),
        "the enabled MPI fixture exercises Linux shared-object linking",
    )
    def test_enabled_tests_off_target_is_default_all_without_rpath(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-rockchip-mpi-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root, languages="CXX")
            artifacts = self._prepare_synthetic_vendor(root)
            module = self._write_pinned_module(
                root, artifacts, "rockit_full", "full"
            )
            configured = self._configure(
                root,
                "enabled-full",
                *self._enabled_definitions(module, artifacts, "rockit_full"),
            )
            self.assertEqual(
                configured.returncode, 0, configured.stdout + configured.stderr
            )
            built = self._build(root, "enabled-full")
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            executable = self._find_link_check(root / "build-enabled-full")

            readelf = shutil.which("readelf")
            self.assertIsNotNone(readelf, "readelf is required for Linux RPATH validation")
            dynamic = subprocess.run(
                [str(readelf), "-d", str(executable)],
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
        "the enabled MPI fixture exercises Linux shared-object linking",
    )
    def test_link_check_requires_representative_sys_mb_ai_ao_symbols(self) -> None:
        missing_variants = (
            ("rockit_missing_sys", "RK_MPI_SYS_Init"),
            ("rockit_missing_mb", "RK_MPI_MB_Handle2VirAddr"),
            ("rockit_missing_ai", "RK_MPI_AI_GetFrame"),
            ("rockit_missing_ao", "RK_MPI_AO_SendFrame"),
        )
        with tempfile.TemporaryDirectory(prefix="boompi-rockchip-mpi-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root, languages="CXX")
            artifacts = self._prepare_synthetic_vendor(root)
            for variant, symbol in missing_variants:
                with self.subTest(symbol=symbol):
                    module = self._write_pinned_module(
                        root, artifacts, variant, variant
                    )
                    case_name = f"missing-{variant}"
                    configured = self._configure(
                        root,
                        case_name,
                        *self._enabled_definitions(module, artifacts, variant),
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


if __name__ == "__main__":
    unittest.main()
