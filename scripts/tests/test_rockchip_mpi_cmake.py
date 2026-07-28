#!/usr/bin/env python3
"""Offline tests for optional Rockchip MPI audio link and HIL targets."""

from __future__ import annotations

import hashlib
import json
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
HIL_SOURCE = (
    REPOSITORY_ROOT
    / "client"
    / "tests"
    / "hil"
    / "rockchip_mpi_audio_hil.cpp"
)
PRIVATE_PATH_MARKER = "private-mpi-vendor-input"
HIL_TARGET = "boompi_rockchip_mpi_audio_hil"
MPI_SENTINEL_ENV = "BOOMPI_MPI_SENTINEL"

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
        if not HIL_SOURCE.is_file():
            raise AssertionError(f"MPI audio HIL source is missing: {HIL_SOURCE}")

    def _write_fixture_project(self, root: Path, *, languages: str = "NONE") -> None:
        source = root / "source"
        source.mkdir()
        fixture = """\
cmake_minimum_required(VERSION 3.21)
project(boompi_rockchip_mpi_gate_fixture LANGUAGES @LANGUAGES@)

if(TEST_ENABLE_CTEST)
  include(CTest)
  enable_testing()
endif()

if(NOT DEFINED BOOMPI_AUDIO_VENDOR_MODULE)
  message(FATAL_ERROR "BOOMPI_AUDIO_VENDOR_MODULE is required")
endif()
include("${BOOMPI_AUDIO_VENDOR_MODULE}")

if(TEST_CONFIGURE_MPI_LINK_CHECK)
  # Platform and feasibility fail-closed behavior is tested through the public
  # entry point separately. This calls only the dependency constructor with
  # host-built stand-ins whose hashes are pinned in a temporary module copy.
  _boompi_configure_rockchip_mpi_audio_dependency()
elseif(TEST_CONFIGURE_MPI_HIL)
  _boompi_configure_rockchip_mpi_audio_hil()
else()
  boompi_configure_audio_vendor_dependencies()
endif()

if(TEST_EXPECT_NO_MPI_TARGET AND TARGET boompi_rockchip_mpi_audio_link_check)
  message(FATAL_ERROR "MPI link check must not exist while disabled")
endif()
if(TEST_EXPECT_MPI_TARGET AND NOT TARGET boompi_rockchip_mpi_audio_link_check)
  message(FATAL_ERROR "MPI link-check target was not created")
endif()
if(TEST_EXPECT_NO_HIL_TARGET AND TARGET boompi_rockchip_mpi_audio_hil)
  message(FATAL_ERROR "MPI audio HIL target must not exist while disabled")
endif()
if(TEST_EXPECT_HIL_TARGET AND NOT TARGET boompi_rockchip_mpi_audio_hil)
  message(FATAL_ERROR "MPI audio HIL target was not created")
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
        self,
        root: Path,
        case_name: str,
        *,
        target: str | None = None,
        environment: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        command = [
            str(self.cmake),
            "--build",
            str(root / f"build-{case_name}"),
            "--config",
            "Debug",
        ]
        if target is not None:
            command.extend(("--target", target))
        return subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            env=environment,
            timeout=60,
        )

    def _install(
        self,
        root: Path,
        case_name: str,
        prefix: Path,
        *,
        environment: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                str(self.cmake),
                "--install",
                str(root / f"build-{case_name}"),
                "--config",
                "Debug",
                "--prefix",
                str(prefix),
            ],
            check=False,
            capture_output=True,
            text=True,
            env=environment,
            timeout=60,
        )

    def _ctest(
        self,
        root: Path,
        case_name: str,
        *arguments: str,
        environment: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        ctest = shutil.which("ctest")
        if ctest is None:
            self.skipTest("CTest executable was not found")
        return subprocess.run(
            [
                ctest,
                "--test-dir",
                str(root / f"build-{case_name}"),
                "-C",
                "Debug",
                *arguments,
            ],
            check=False,
            capture_output=True,
            text=True,
            env=environment,
            timeout=60,
        )

    def _write_synthetic_headers(self, include_dir: Path) -> None:
        include_dir.mkdir(parents=True)
        headers = {
            "rk_type.h": """\
#pragma once
typedef signed char RK_S8;
typedef unsigned char RK_U8;
typedef short RK_S16;
typedef unsigned short RK_U16;
typedef int RK_S32;
typedef unsigned int RK_U32;
typedef long long RK_S64;
typedef unsigned long long RK_U64;
typedef int RK_BOOL;
typedef char RK_CHAR;
typedef void RK_VOID;
#define RK_FALSE 0
#define RK_TRUE 1
#define RK_SUCCESS 0
#define RK_FAILURE (-1)
#define RK_NULL 0
""",
            "rk_common.h": """\
#pragma once
#include "rk_type.h"
""",
            "rk_comm_mb.h": """\
#pragma once
#include "rk_common.h"
typedef RK_VOID* MB_BLK;
typedef RK_VOID (*MB_FREE_CB)(RK_VOID* opaque);
typedef struct rkMB_EXT_CONFIG_S {
  MB_FREE_CB pFreeCB;
  RK_VOID* pOpaque;
  RK_U8* pu8VirAddr;
  RK_U64 u64Size;
} MB_EXT_CONFIG_S;
""",
            "rk_comm_aio.h": """\
#pragma once
#include "rk_comm_mb.h"
typedef RK_S32 AUDIO_DEV;
typedef RK_S32 AI_CHN;
typedef RK_S32 AO_CHN;
typedef enum rkAUDIO_SAMPLE_RATE_E {
  AUDIO_SAMPLE_RATE_8000 = 8000,
  AUDIO_SAMPLE_RATE_16000 = 16000,
  AUDIO_SAMPLE_RATE_48000 = 48000
} AUDIO_SAMPLE_RATE_E;
typedef enum rkAUDIO_BIT_WIDTH_E {
  AUDIO_BIT_WIDTH_8 = 0,
  AUDIO_BIT_WIDTH_16 = 1,
  AUDIO_BIT_WIDTH_24 = 2
} AUDIO_BIT_WIDTH_E;
typedef enum rkAUDIO_SOUND_MODE_E {
  AUDIO_SOUND_MODE_MONO = 0,
  AUDIO_SOUND_MODE_STEREO = 1
} AUDIO_SOUND_MODE_E;
typedef enum rkAUDIO_LOOPBACK_MODE_E {
  AUDIO_LOOPBACK_NONE = 0
} AUDIO_LOOPBACK_MODE_E;
typedef struct rkAUDIO_SOUND_CARD_S {
  RK_U32 channels;
  RK_U32 sampleRate;
  AUDIO_BIT_WIDTH_E bitWidth;
} AUDIO_SOUND_CARD_S;
typedef struct rkAIO_ATTR_S {
  AUDIO_SOUND_CARD_S soundCard;
  AUDIO_SAMPLE_RATE_E enSamplerate;
  AUDIO_BIT_WIDTH_E enBitwidth;
  AUDIO_SOUND_MODE_E enSoundmode;
  RK_U32 u32FrmNum;
  RK_U32 u32PtNumPerFrm;
  RK_U32 u32EXFlag;
  RK_U32 u32ChnCnt;
  RK_U8 u8CardName[64];
} AIO_ATTR_S;
typedef struct rkAI_CHN_PARAM_S {
  RK_S32 s32UsrFrmDepth;
  AUDIO_LOOPBACK_MODE_E enLoopbackMode;
} AI_CHN_PARAM_S;
typedef struct rkAO_CHN_PARAM_S {
  RK_U32 u32ChnBufSize;
  AUDIO_LOOPBACK_MODE_E enLoopbackMode;
} AO_CHN_PARAM_S;
typedef struct rkAUDIO_FRAME_S {
  MB_BLK pMbBlk;
  RK_U32 u32Len;
  RK_U64 u64TimeStamp;
  RK_U32 u32Seq;
  AUDIO_BIT_WIDTH_E enBitWidth;
  AUDIO_SOUND_MODE_E enSoundMode;
  RK_S32 s32SampleRate;
  RK_BOOL bBypassMbBlk;
} AUDIO_FRAME_S;
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
RK_U64 RK_MPI_MB_GetSize(MB_BLK block);
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
#include <cstdio>
#include <cstdlib>

#include "rk_mpi_ai.h"
#include "rk_mpi_ao.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"

extern "C" int boompi_mpp_anchor();
extern "C" int boompi_rga_anchor();

namespace {

void boompi_record_call(const char* name) {
  const char* path = std::getenv("BOOMPI_MPI_SENTINEL");
  if (path == nullptr || path[0] == '\\0') {
    return;
  }
  if (std::FILE* output = std::fopen(path, "ab")) {
    std::fputs(name, output);
    std::fputc('\\n', output);
    std::fclose(output);
  }
}

}  // namespace

#if !defined(BOOMPI_OMIT_SYS)
extern "C" RK_S32 RK_MPI_SYS_Init(RK_VOID) {
  boompi_record_call("RK_MPI_SYS_Init");
  return boompi_mpp_anchor() + boompi_rga_anchor();
}
#endif
extern "C" RK_S32 RK_MPI_SYS_Exit(RK_VOID) {
  boompi_record_call("RK_MPI_SYS_Exit");
  return 0;
}
extern "C" RK_S32 RK_MPI_SYS_CreateMB(MB_BLK*, MB_EXT_CONFIG_S*) {
  boompi_record_call("RK_MPI_SYS_CreateMB");
  return 0;
}
#if !defined(BOOMPI_OMIT_MB)
extern "C" RK_VOID* RK_MPI_MB_Handle2VirAddr(MB_BLK block) {
  boompi_record_call("RK_MPI_MB_Handle2VirAddr");
  return block;
}
#endif
#if !defined(BOOMPI_OMIT_MB_GET_SIZE)
extern "C" RK_U64 RK_MPI_MB_GetSize(MB_BLK) {
  boompi_record_call("RK_MPI_MB_GetSize");
  return 0;
}
#endif
extern "C" RK_S32 RK_MPI_MB_ReleaseMB(MB_BLK) {
  boompi_record_call("RK_MPI_MB_ReleaseMB");
  return 0;
}

extern "C" RK_S32 RK_MPI_AI_SetPubAttr(AUDIO_DEV, const AIO_ATTR_S*) {
  boompi_record_call("RK_MPI_AI_SetPubAttr");
  return 0;
}
extern "C" RK_S32 RK_MPI_AI_Enable(AUDIO_DEV) {
  boompi_record_call("RK_MPI_AI_Enable");
  return 0;
}
extern "C" RK_S32 RK_MPI_AI_EnableChn(AUDIO_DEV, AI_CHN) {
  boompi_record_call("RK_MPI_AI_EnableChn");
  return 0;
}
extern "C" RK_S32 RK_MPI_AI_SetChnParam(
    AUDIO_DEV, AI_CHN, const AI_CHN_PARAM_S*) {
  boompi_record_call("RK_MPI_AI_SetChnParam");
  return 0;
}
#if !defined(BOOMPI_OMIT_AI)
extern "C" RK_S32 RK_MPI_AI_GetFrame(
    AUDIO_DEV, AI_CHN, AUDIO_FRAME_S*, AEC_FRAME_S*, RK_S32) {
  boompi_record_call("RK_MPI_AI_GetFrame");
  return 0;
}
#endif
extern "C" RK_S32 RK_MPI_AI_ReleaseFrame(
    AUDIO_DEV, AI_CHN, const AUDIO_FRAME_S*, const AEC_FRAME_S*) {
  boompi_record_call("RK_MPI_AI_ReleaseFrame");
  return 0;
}
extern "C" RK_S32 RK_MPI_AI_DisableChn(AUDIO_DEV, AI_CHN) {
  boompi_record_call("RK_MPI_AI_DisableChn");
  return 0;
}
extern "C" RK_S32 RK_MPI_AI_Disable(AUDIO_DEV) {
  boompi_record_call("RK_MPI_AI_Disable");
  return 0;
}

extern "C" RK_S32 RK_MPI_AO_SetPubAttr(AUDIO_DEV, const AIO_ATTR_S*) {
  boompi_record_call("RK_MPI_AO_SetPubAttr");
  return 0;
}
extern "C" RK_S32 RK_MPI_AO_Enable(AUDIO_DEV) {
  boompi_record_call("RK_MPI_AO_Enable");
  return 0;
}
extern "C" RK_S32 RK_MPI_AO_EnableChn(AUDIO_DEV, AO_CHN) {
  boompi_record_call("RK_MPI_AO_EnableChn");
  return 0;
}
extern "C" RK_S32 RK_MPI_AO_SetChnParams(
    AUDIO_DEV, AO_CHN, const AO_CHN_PARAM_S*) {
  boompi_record_call("RK_MPI_AO_SetChnParams");
  return 0;
}
#if !defined(BOOMPI_OMIT_AO)
extern "C" RK_S32 RK_MPI_AO_SendFrame(
    AUDIO_DEV, AO_CHN, const AUDIO_FRAME_S*, RK_S32) {
  boompi_record_call("RK_MPI_AO_SendFrame");
  return 0;
}
#endif
extern "C" RK_S32 RK_MPI_AO_WaitEos(AUDIO_DEV, AO_CHN, RK_S32) {
  boompi_record_call("RK_MPI_AO_WaitEos");
  return 0;
}
extern "C" RK_S32 RK_MPI_AO_DisableChn(AUDIO_DEV, AO_CHN) {
  boompi_record_call("RK_MPI_AO_DisableChn");
  return 0;
}
extern "C" RK_S32 RK_MPI_AO_Disable(AUDIO_DEV) {
  boompi_record_call("RK_MPI_AO_Disable");
  return 0;
}
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
add_synthetic_rockit(rockit_missing_mb_get_size rockit_missing_mb_get_size)
target_compile_definitions(rockit_missing_mb_get_size PRIVATE
  BOOMPI_OMIT_MB_GET_SIZE=1)
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
            "rockit_missing_mb_get_size": output / "librockit_missing_mb_get_size.so",
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
            "rockit_missing_mb_get_size",
            "rockit_missing_ai",
            "rockit_missing_ao",
        ):
            variant_dir = private_root / variant
            variant_dir.mkdir()
            destination = variant_dir / "librockit.so"
            shutil.copy2(built_artifacts[variant], destination)
            artifacts[variant] = destination
        return artifacts

    def _write_synthetic_target_environment(self, root: Path) -> list[str]:
        self.assertTrue(
            sys.platform.startswith("linux"),
            "the synthetic target compiler wrapper requires a POSIX host",
        )
        compiler_path = shutil.which("c++") or shutil.which("g++")
        self.assertIsNotNone(compiler_path, "a host GNU C++ compiler is required")

        compiler = (
            root
            / "toolchain"
            / "arm-rockchip830-linux-uclibcgnueabihf-g++"
        )
        compiler.parent.mkdir()
        compiler.write_text(
            """\
#!/usr/bin/env python3
import subprocess
import sys

real_compiler = @REAL_COMPILER@
filtered = []
skip_next = False
for argument in sys.argv[1:]:
    if skip_next:
        skip_next = False
        continue
    if argument in ("--sysroot", "-isysroot"):
        skip_next = True
        continue
    if argument.startswith("--sysroot="):
        continue
    filtered.append(argument)
raise SystemExit(subprocess.run([real_compiler, *filtered], check=False).returncode)
""".replace("@REAL_COMPILER@", repr(str(Path(compiler_path).resolve()))),
            encoding="utf-8",
        )
        compiler.chmod(0o755)

        sysroot = root / "synthetic-rv1106-sysroot"
        loader = sysroot / "lib" / "ld-uClibc.so.0"
        loader.parent.mkdir(parents=True)
        loader.write_bytes(b"synthetic uClibc loader identity; never executed\n")
        return [
            "-DBOOMPI_TARGET_RV1106=ON",
            "-DCMAKE_SYSTEM_NAME:STRING=Linux",
            "-DCMAKE_SYSTEM_PROCESSOR:STRING=arm",
            f"-DCMAKE_CXX_COMPILER:FILEPATH={compiler}",
            f"-DCMAKE_SYSROOT:PATH={sysroot}",
        ]

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
        hil_dir = module_root / "tests" / "hil"
        module_dir.mkdir(parents=True)
        link_dir.mkdir(parents=True)
        hil_dir.mkdir(parents=True)

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
        shutil.copy2(HIL_SOURCE, hil_dir / HIL_SOURCE.name)
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

    def _hil_enabled_definitions(
        self,
        root: Path,
        module: Path,
        artifacts: dict[str, Path],
    ) -> list[str]:
        return [
            f"-DBOOMPI_AUDIO_VENDOR_MODULE:FILEPATH={module}",
            "-DBOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO=ON",
            "-DBOOMPI_BUILD_ROCKCHIP_MPI_AUDIO_HIL=ON",
            "-DBOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON",
            "-DBOOMPI_BUILD_TESTS=OFF",
            "-DBUILD_TESTING=ON",
            "-DCMAKE_BUILD_TYPE:STRING=Debug",
            "-DTEST_ENABLE_CTEST=ON",
            "-DTEST_EXPECT_MPI_TARGET=ON",
            "-DTEST_EXPECT_HIL_TARGET=ON",
            *self._write_synthetic_target_environment(root),
            f"-DBOOMPI_ROCKCHIP_MPI_INCLUDE_DIR:PATH={artifacts['include']}",
            f"-DBOOMPI_ROCKCHIP_MPI_ROCKIT_LIBRARY:FILEPATH={artifacts['rockit_full']}",
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

    def _hil_executables(self, build: Path) -> list[Path]:
        names = {HIL_TARGET, f"{HIL_TARGET}.exe"}
        return [
            candidate
            for candidate in build.rglob(f"{HIL_TARGET}*")
            if candidate.is_file() and candidate.name in names
        ]

    def _find_hil_executable(self, build: Path) -> Path:
        candidates = self._hil_executables(build)
        self.assertEqual(
            len(candidates), 1, "MPI audio HIL executable was not built exactly once"
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
                "-DTEST_EXPECT_NO_HIL_TARGET=ON",
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

    def test_hil_requires_mpi_dependency_before_vendor_paths(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-rockchip-mpi-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root)
            private = root / PRIVATE_PATH_MARKER
            configured = self._configure(
                root,
                "hil-without-mpi",
                "-DBOOMPI_BUILD_ROCKCHIP_MPI_AUDIO_HIL=ON",
                f"-DBOOMPI_ROCKCHIP_MPI_INCLUDE_DIR:PATH={private / 'include'}",
                f"-DBOOMPI_ROCKCHIP_MPI_ROCKIT_LIBRARY:FILEPATH={private / 'librockit.so'}",
                f"-DBOOMPI_ROCKCHIP_MPI_MPP_LIBRARY:FILEPATH={private / 'libmpp.so'}",
                f"-DBOOMPI_ROCKCHIP_MPI_RGA_LIBRARY:FILEPATH={private / 'librga.so'}",
            )
        output = configured.stdout + configured.stderr
        self.assertNotEqual(configured.returncode, 0, output)
        self.assertIn("BOOMPI_BUILD_ROCKCHIP_MPI_AUDIO_HIL", output)
        self.assertIn("BOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO", output)
        self.assertNotIn(PRIVATE_PATH_MARKER, output)

    def test_hil_rejects_host_target_boolean_spoof_before_vendor_paths(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-rockchip-mpi-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root)
            private = root / PRIVATE_PATH_MARKER
            configured = self._configure(
                root,
                "hil-host-spoof",
                "-DBOOMPI_TARGET_RV1106=ON",
                "-DCMAKE_SYSTEM_PROCESSOR:STRING=arm",
                "-DBOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO=ON",
                "-DBOOMPI_BUILD_ROCKCHIP_MPI_AUDIO_HIL=ON",
                f"-DBOOMPI_ROCKCHIP_MPI_INCLUDE_DIR:PATH={private / 'include'}",
                f"-DBOOMPI_ROCKCHIP_MPI_ROCKIT_LIBRARY:FILEPATH={private / 'librockit.so'}",
                f"-DBOOMPI_ROCKCHIP_MPI_MPP_LIBRARY:FILEPATH={private / 'libmpp.so'}",
                f"-DBOOMPI_ROCKCHIP_MPI_RGA_LIBRARY:FILEPATH={private / 'librga.so'}",
            )
        output = configured.stdout + configured.stderr
        self.assertNotEqual(configured.returncode, 0, output)
        self.assertIn("cross-compiled Linux/ARM", output)
        self.assertNotIn(PRIVATE_PATH_MARKER, output)

    def test_direct_hil_helper_repeats_host_gate_before_vendor_paths(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-rockchip-mpi-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root)
            private = root / PRIVATE_PATH_MARKER
            configured = self._configure(
                root,
                "direct-hil-host-spoof",
                "-DTEST_CONFIGURE_MPI_HIL=ON",
                "-DBOOMPI_TARGET_RV1106=ON",
                "-DCMAKE_SYSTEM_PROCESSOR:STRING=arm",
                "-DBOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO=ON",
                "-DBOOMPI_BUILD_ROCKCHIP_MPI_AUDIO_HIL=ON",
                f"-DBOOMPI_ROCKCHIP_MPI_INCLUDE_DIR:PATH={private / 'include'}",
                f"-DBOOMPI_ROCKCHIP_MPI_ROCKIT_LIBRARY:FILEPATH={private / 'librockit.so'}",
                f"-DBOOMPI_ROCKCHIP_MPI_MPP_LIBRARY:FILEPATH={private / 'libmpp.so'}",
                f"-DBOOMPI_ROCKCHIP_MPI_RGA_LIBRARY:FILEPATH={private / 'librga.so'}",
            )
        output = configured.stdout + configured.stderr
        self.assertNotEqual(configured.returncode, 0, output)
        self.assertIn("cross-compiled Linux/ARM", output)
        self.assertNotIn(PRIVATE_PATH_MARKER, output)

    @unittest.skipUnless(
        sys.platform.startswith("linux"),
        "the direct-constructor fixture links Linux synthetic shared objects",
    )
    def test_dependency_constructor_never_creates_opted_in_hil(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-rockchip-mpi-cmake-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root, languages="CXX")
            artifacts = self._prepare_synthetic_vendor(root)
            module = self._write_pinned_module(
                root, artifacts, "rockit_full", "constructor-hil-opt-in"
            )
            configured = self._configure(
                root,
                "constructor-hil-opt-in",
                *self._enabled_definitions(module, artifacts, "rockit_full"),
                "-DBOOMPI_BUILD_ROCKCHIP_MPI_AUDIO_HIL=ON",
                "-DTEST_EXPECT_NO_HIL_TARGET=ON",
            )
        self.assertEqual(
            configured.returncode, 0, configured.stdout + configured.stderr
        )

    def test_hil_source_is_bounded_raw_mpi_without_external_mutators(self) -> None:
        source = HIL_SOURCE.read_text(encoding="utf-8")

        forbidden_patterns = {
            "Rockchip wait-forever token": r"\bRK_WAIT_FOREVER\b",
            "shell execution": r"\b(?:system|popen|execl|execv|fork)\s*\(",
            "process killer": r"\bkillall\b|\bpkill\b",
            "external mixer utility": r"\b(?:amixer|tinymix|alsactl)\b",
            "ALSA mixer API": r"\bsnd_mixer_",
            "MPI bind graph": r"\bRK_MPI_SYS_(?:Bind|UnBind)\b",
            "MPI VQE": r"\bRK_MPI_(?:AI|AO)_(?:SetVqeAttr|EnableVqe|DisableVqe)\b",
            "MPI resampler": r"\bRK_MPI_(?:AI|AO)_(?:EnableReSmp|DisableReSmp)\b",
            "encoded media graph": r"\bRK_MPI_(?:AENC|ADEC)_",
            "Rockchip mixer API": r"\bRK_MPI_AMIX_",
        }
        for contract, pattern in forbidden_patterns.items():
            with self.subTest(contract=contract):
                self.assertNotRegex(source, pattern)

        for option in (
            "--execute",
            "--allow-ai-capture",
            "--allow-ao-playback",
        ):
            self.assertIn(option, source)

        for api in (
            "RK_MPI_AI_GetFrame",
            "RK_MPI_AO_SendFrame",
            "RK_MPI_AO_WaitEos",
        ):
            calls = re.findall(
                rf"{re.escape(api)}\s*\((.*?)\)\s*;", source, flags=re.DOTALL
            )
            self.assertGreaterEqual(len(calls), 1, f"missing bounded call: {api}")
            for arguments in calls:
                self.assertNotRegex(
                    arguments,
                    r"(?<![A-Za-z0-9_])-1(?![A-Za-z0-9_])|"
                    r"\bRK_WAIT_FOREVER\b|\b(?:UINT_MAX|INT_MAX)\b|"
                    r"numeric_limits[^;]*max\s*\(",
                )

    @unittest.skipUnless(
        sys.platform.startswith("linux"),
        "the enabled HIL fixture uses a Linux compiler wrapper and shared objects",
    )
    def test_hil_is_explicit_noninstalled_nonctest_and_never_auto_runs(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-rockchip-mpi-hil-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root, languages="CXX")
            artifacts = self._prepare_synthetic_vendor(root)
            module = self._write_pinned_module(
                root, artifacts, "rockit_full", "hil-public-gate"
            )
            case_name = "hil-public-gate"
            sentinel = root / "mpi-invoked.sentinel"
            environment = dict(environ)
            environment[MPI_SENTINEL_ENV] = str(sentinel)

            configured = self._configure(
                root,
                case_name,
                *self._hil_enabled_definitions(root, module, artifacts),
            )
            self.assertEqual(
                configured.returncode, 0, configured.stdout + configured.stderr
            )

            target_help = self._build(
                root, case_name, target="help", environment=environment
            )
            self.assertEqual(
                target_help.returncode, 0, target_help.stdout + target_help.stderr
            )
            self.assertIn(HIL_TARGET, target_help.stdout + target_help.stderr)
            self.assertFalse(sentinel.exists(), "target discovery executed MPI")

            built_default = self._build(root, case_name, environment=environment)
            self.assertEqual(
                built_default.returncode,
                0,
                built_default.stdout + built_default.stderr,
            )
            self._find_link_check(root / f"build-{case_name}")
            self.assertEqual(
                self._hil_executables(root / f"build-{case_name}"), [],
                "EXCLUDE_FROM_ALL HIL was produced by the default build",
            )
            self.assertFalse(sentinel.exists(), "default build executed MPI")

            built_hil = self._build(
                root, case_name, target=HIL_TARGET, environment=environment
            )
            self.assertEqual(
                built_hil.returncode, 0, built_hil.stdout + built_hil.stderr
            )
            executable = self._find_hil_executable(root / f"build-{case_name}")
            self.assertFalse(sentinel.exists(), "explicit build executed the HIL")

            listed_tests = self._ctest(
                root, case_name, "-N", environment=environment
            )
            self.assertEqual(
                listed_tests.returncode,
                0,
                listed_tests.stdout + listed_tests.stderr,
            )
            self.assertNotIn(HIL_TARGET, listed_tests.stdout + listed_tests.stderr)
            ran_tests = self._ctest(
                root, case_name, "--output-on-failure", environment=environment
            )
            self.assertEqual(
                ran_tests.returncode, 0, ran_tests.stdout + ran_tests.stderr
            )
            self.assertNotIn(HIL_TARGET, ran_tests.stdout + ran_tests.stderr)
            self.assertFalse(sentinel.exists(), "CTest executed the HIL")

            install_prefix = root / "install-tree"
            installed = self._install(
                root,
                case_name,
                install_prefix,
                environment=environment,
            )
            self.assertEqual(installed.returncode, 0, installed.stdout + installed.stderr)
            installed_hil = [
                path
                for path in install_prefix.rglob("*")
                if path.is_file() and HIL_TARGET in path.name
            ]
            self.assertEqual(installed_hil, [])
            manifest = root / f"build-{case_name}" / "install_manifest.txt"
            if manifest.is_file():
                self.assertNotIn(HIL_TARGET, manifest.read_text(encoding="utf-8"))
            self.assertFalse(sentinel.exists(), "install executed the HIL")

            readelf = shutil.which("readelf")
            self.assertIsNotNone(readelf, "readelf is required for ELF validation")
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
            self.assertNotIn(PRIVATE_PATH_MARKER.encode(), executable.read_bytes())

            library_directories = {
                str(root / "synthetic-mpi-vendor-build" / "out"),
                str(artifacts["rockit_full"].parent),
                str(artifacts["mpp"].parent),
                str(artifacts["rga"].parent),
            }
            inherited_library_path = environment.get("LD_LIBRARY_PATH", "")
            if inherited_library_path:
                library_directories.add(inherited_library_path)
            run_environment = dict(environment)
            run_environment["LD_LIBRARY_PATH"] = ":".join(
                sorted(library_directories)
            )
            for arguments, expected_returncode in (((), 2), (("--help",), 0)):
                with self.subTest(arguments=arguments):
                    sentinel.unlink(missing_ok=True)
                    completed = subprocess.run(
                        [str(executable), *arguments],
                        check=False,
                        capture_output=True,
                        text=True,
                        env=run_environment,
                        timeout=5,
                    )
                    self.assertEqual(
                        completed.returncode,
                        expected_returncode,
                        completed.stdout + completed.stderr,
                    )
                    self.assertFalse(
                        sentinel.exists(), "usage/help path invoked Rockchip MPI"
                    )

            dry_run_artifact = root / "dry-run-artifact"
            sentinel.unlink(missing_ok=True)
            dry_run = subprocess.run(
                [
                    str(executable),
                    "--ai-card",
                    "hw:0,0",
                    "--ao-card",
                    "hw:0,0",
                    "--ai-device",
                    "0",
                    "--ai-channel",
                    "0",
                    "--ao-device",
                    "0",
                    "--ao-channel",
                    "0",
                    "--artifact-dir",
                    str(dry_run_artifact),
                ],
                check=False,
                capture_output=True,
                text=True,
                env=run_environment,
                timeout=5,
            )
            self.assertEqual(dry_run.returncode, 0, dry_run.stdout + dry_run.stderr)
            dry_run_document = json.loads(dry_run.stdout)
            self.assertEqual(
                dry_run_document["occupancy_scan_targets"],
                "configured_pcm_and_all_dev_mpi",
            )
            self.assertFalse(
                dry_run_document["occupancy_scan_is_exclusive_reservation"]
            )
            self.assertFalse(sentinel.exists(), "dry-run invoked Rockchip MPI")
            self.assertFalse(
                dry_run_artifact.exists(), "dry-run created an artifact directory"
            )

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
            ("rockit_missing_mb_get_size", "RK_MPI_MB_GetSize"),
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
