#!/usr/bin/env python3
"""Offline Linux tests for the opt-in Rockchip fixed-frame 3A HIL probe."""

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
    REPOSITORY_ROOT / "client" / "tests" / "link" / "rockchip_3a_link_check.cpp"
)
HIL_SOURCE = (
    REPOSITORY_ROOT / "client" / "tests" / "hil" / "rockchip_3a_hil.cpp"
)
PRODUCTION_DSP_SOURCE = (
    REPOSITORY_ROOT
    / "client"
    / "src"
    / "platform"
    / "rv1106"
    / "rockchip_voice_dsp.cpp"
)
PRODUCTION_INCLUDE_DIR = REPOSITORY_ROOT / "client" / "include"

HIL_TARGET = "boompi_rockchip_3a_hil"
LINK_CHECK_TARGET = "boompi_rockchip_3a_link_check"
PRIVATE_PATH_MARKER = "private-rockchip-3a-input"
SENTINEL_ENV = "BOOMPI_3A_FAKE_SENTINEL"
MODE_ENV = "BOOMPI_3A_FAKE_MODE"
VENDOR_DEBUG_PATH_VARIABLES = (
    "PATH_RX_IN",
    "PATH_RX_OUT",
    "PATH_TX_IN_MIC",
    "PATH_TX_IN_REF",
    "PATH_TX_OUT_MDF",
    "PATH_TX_OUT_MDFFAST",
)
LOADER_OVERRIDE_VARIABLES = ("LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT")

PIN_VARIABLES = {
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


@unittest.skipUnless(
    sys.platform.startswith("linux"),
    "the fake Rockchip 3A fixture links and runs Linux shared objects",
)
class Rockchip3aHilTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cmake = _find_cmake()
        if cls.cmake is None:
            raise unittest.SkipTest("CMake executable was not found")
        cls.host_cxx = shutil.which("c++") or shutil.which("g++")
        if cls.host_cxx is None:
            raise unittest.SkipTest("a host GNU C++ compiler was not found")
        for required in (
            VENDOR_MODULE,
            LINK_CHECK_SOURCE,
            HIL_SOURCE,
            PRODUCTION_DSP_SOURCE,
        ):
            if not required.is_file():
                raise AssertionError(f"required Rockchip 3A input is missing: {required}")

    def _write_fixture_project(self, root: Path, *, languages: str = "NONE") -> None:
        source = root / "source"
        source.mkdir()
        cmake_lists = """\
cmake_minimum_required(VERSION 3.21)
project(boompi_rockchip_3a_hil_fixture LANGUAGES @LANGUAGES@)

if(TEST_ENABLE_CTEST)
  include(CTest)
  enable_testing()
endif()

if(NOT DEFINED BOOMPI_AUDIO_VENDOR_MODULE)
  message(FATAL_ERROR "BOOMPI_AUDIO_VENDOR_MODULE is required")
endif()
include("${BOOMPI_AUDIO_VENDOR_MODULE}")

if(TEST_CONFIGURE_HIL_DIRECT)
  _boompi_configure_rockchip_3a_hil()
else()
  boompi_configure_audio_vendor_dependencies()
endif()

if(TEST_HIL_VENDOR_LIBRARY_PATH AND TARGET boompi_rockchip_3a_hil)
  target_compile_definitions(boompi_rockchip_3a_hil PRIVATE
    "BOOMPI_ROCKCHIP_3A_HIL_TEST_VENDOR_LIBRARY_PATH=\\\"${TEST_HIL_VENDOR_LIBRARY_PATH}\\\"")
endif()

if(TEST_EXPECT_NO_LINK_TARGET AND TARGET boompi_rockchip_3a_link_check)
  message(FATAL_ERROR "Rockchip 3A link-check target must not exist")
endif()
if(TEST_EXPECT_LINK_TARGET AND NOT TARGET boompi_rockchip_3a_link_check)
  message(FATAL_ERROR "Rockchip 3A link-check target was not created")
endif()
if(TEST_EXPECT_NO_HIL_TARGET AND TARGET boompi_rockchip_3a_hil)
  message(FATAL_ERROR "Rockchip 3A HIL target must not exist")
endif()
if(TEST_EXPECT_HIL_TARGET AND NOT TARGET boompi_rockchip_3a_hil)
  message(FATAL_ERROR "Rockchip 3A HIL target was not created")
endif()
"""
        (source / "CMakeLists.txt").write_text(
            cmake_lists.replace("@LANGUAGES@", languages), encoding="utf-8"
        )

    def _copy_module_and_sources(
        self,
        root: Path,
        case_name: str,
        pinned_inputs: dict[str, Path] | None = None,
    ) -> Path:
        module_root = root / f"fixture-module-{case_name}" / "client"
        module_dir = module_root / "cmake"
        link_dir = module_root / "tests" / "link"
        hil_dir = module_root / "tests" / "hil"
        module_dir.mkdir(parents=True)
        link_dir.mkdir(parents=True)
        hil_dir.mkdir(parents=True)

        module_text = VENDOR_MODULE.read_text(encoding="utf-8")
        if pinned_inputs is not None:
            for variable, logical_name in PIN_VARIABLES.items():
                digest = hashlib.sha256(
                    pinned_inputs[logical_name].read_bytes()
                ).hexdigest()
                pattern = re.compile(
                    rf'(set\({re.escape(variable)}\s+")[0-9A-Fa-f]{{64}}("\))'
                )
                module_text, replacements = pattern.subn(
                    lambda match, value=digest: (
                        f"{match.group(1)}{value}{match.group(2)}"
                    ),
                    module_text,
                )
                self.assertEqual(replacements, 1, f"pin not found: {variable}")

        module = module_dir / VENDOR_MODULE.name
        module.write_text(module_text, encoding="utf-8")
        shutil.copy2(LINK_CHECK_SOURCE, link_dir / LINK_CHECK_SOURCE.name)
        shutil.copy2(HIL_SOURCE, hil_dir / HIL_SOURCE.name)
        return module

    def _configure(
        self,
        root: Path,
        case_name: str,
        module: Path,
        *definitions: str,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                str(self.cmake),
                "-S",
                str(root / "source"),
                "-B",
                str(root / f"build-{case_name}"),
                f"-DBOOMPI_AUDIO_VENDOR_MODULE:FILEPATH={module}",
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

    def _write_target_compiler_wrapper(self, root: Path) -> list[str]:
        compiler = (
            root / "toolchain" / "arm-rockchip830-linux-uclibcgnueabihf-g++"
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
""".replace("@REAL_COMPILER@", repr(str(Path(self.host_cxx).resolve()))),
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

    def _prepare_fake_vendor(self, root: Path) -> dict[str, Path]:
        source = root / "fake-rockchip-3a-source"
        include_dir = source / "include"
        include_dir.mkdir(parents=True)
        (include_dir / "rkaudio_preprocess.h").write_text(
            """\
#pragma once

#include <stdio.h>
#include <stdlib.h>

#define RKAUDIO_EN_AEC (1 << 0)
#define RKAUDIO_EN_BF (1 << 1)
#define EN_DELAY (1 << 0)
#define EN_Fastaec 1
#define EN_Dereverberation 4
#define EN_AES 16
#define EN_Agc 32
#define EN_Anr 64
#define EN_Fix 512
#define EN_STDT 1024
#define EN_HOWLING 16384

typedef struct SKVAECParameter {
  int pos;
  int drop_ref_channel;
  int model_aec_en;
  int delay_len;
  int look_ahead;
  short* Array_list;
  short filter_len;
  void* delay_para;
} SKVAECParameter;

typedef struct RKAudioDereverbParam {
  int rlsLg;
  int curveLg;
  int delay;
  float forgetting;
  float T60;
  float coCoeff;
} RKAudioDereverbParam;

typedef struct RKAudioAESParameter {
  float Beta_Up;
  float Beta_Down;
  float Beta_Up_Low;
  float Beta_Down_Low;
  short low_freq;
  short high_freq;
  short THD_Flag;
  short HARD_Flag;
  float LimitRatio[2][3];
  short ThdSplitFreq[4][2];
  float ThdSupDegree[4][10];
  short HardSplitFreq[5][2];
  float HardThreshold[4];
} RKAudioAESParameter;

typedef struct SKVANRParam {
  float noiseFactor;
  int swU;
  float PsiMin;
  float PsiMax;
  float fGmin;
  short Sup_Freq1;
  short Sup_Freq2;
  float Sup_Energy1;
  float Sup_Energy2;
  short InterV;
  float BiasMin;
  short UpdateFrm;
  float NPreGammaThr;
  float NPreZetaThr;
  float SabsGammaThr0;
  float SabsGammaThr1;
  float InfSmooth;
  float ProbSmooth;
  float CompCoeff;
  float PrioriMin;
  float PostMax;
  float PrioriRatio;
  float PrioriRatioLow;
  int SplitBand;
  float PrioriSmooth;
  short TranMode;
} SKVANRParam;

typedef struct RKAGCParam {
  float attack_time;
  float release_time;
  float max_gain;
  float max_peak;
  float fRth0;
  float fRk0;
  float fRth1;
  int fs;
  int frmlen;
  float attenuate_time;
  float fRth2;
  float fRk1;
  float fRk2;
  float fLineGainDb;
  int swSmL0;
  int swSmL1;
  int swSmL2;
} RKAGCParam;

typedef struct RKHOWLParam {
  short howlMode;
} RKHOWLParam;

typedef struct RKDTDParam {
  float ksiThd_high;
  float ksiThd_low;
} RKDTDParam;

typedef struct SKVPreprocessParam {
  int model_bf_en;
  int ref_pos;
  int Targ;
  int num_ref_channel;
  int drop_ref_channel;
  void* dereverb_para;
  void* aes_para;
  void* nlp_para;
  void* anr_para;
  void* agc_para;
  void* cng_para;
  void* dtd_para;
  void* eq_para;
  void* howl_para;
  void* doa_para;
} SKVPreprocessParam;

typedef struct RKAUDIOParam {
  int model_en;
  void* aec_param;
  void* bf_param;
  void* rx_param;
  int read_size;
} RKAUDIOParam;

static inline void rkaudio_fake_record(const char* message) {
  const char* path = getenv("BOOMPI_3A_FAKE_SENTINEL");
  if (path == NULL || path[0] == '\\0') {
    return;
  }
  FILE* output = fopen(path, "ab");
  if (output != NULL) {
    fputs(message, output);
    fputc('\\n', output);
    fclose(output);
  }
}

static inline void* rkaudio_aec_param_init(void) {
  SKVAECParameter* parameters =
      (SKVAECParameter*)calloc(1, sizeof(SKVAECParameter));
  if (parameters != NULL) {
    parameters->filter_len = 2;
    parameters->delay_para = calloc(1, 1);
    if (parameters->delay_para == NULL) {
      free(parameters);
      return NULL;
    }
    rkaudio_fake_record("aec_param_init");
  }
  return parameters;
}

static inline void* rkaudio_preprocess_param_init(void) {
  SKVPreprocessParam* parameters =
      (SKVPreprocessParam*)calloc(1, sizeof(SKVPreprocessParam));
  if (parameters == NULL) {
    return NULL;
  }
  parameters->dereverb_para = calloc(1, sizeof(RKAudioDereverbParam));
  parameters->aes_para = calloc(1, sizeof(RKAudioAESParameter));
  parameters->anr_para = calloc(1, sizeof(SKVANRParam));
  parameters->agc_para = calloc(1, sizeof(RKAGCParam));
  parameters->dtd_para = calloc(1, sizeof(RKDTDParam));
  parameters->howl_para = calloc(1, sizeof(RKHOWLParam));
  if (parameters->dereverb_para == NULL || parameters->aes_para == NULL ||
      parameters->anr_para == NULL || parameters->agc_para == NULL ||
      parameters->dtd_para == NULL || parameters->howl_para == NULL) {
    free(parameters->dereverb_para);
    free(parameters->aes_para);
    free(parameters->anr_para);
    free(parameters->agc_para);
    free(parameters->dtd_para);
    free(parameters->howl_para);
    free(parameters);
    return NULL;
  }
  rkaudio_fake_record("bf_param_init");
  return parameters;
}

static inline void rkaudio_param_deinit(RKAUDIOParam* parameters) {
  if (parameters == NULL) {
    return;
  }
  SKVPreprocessParam* beamforming =
      (SKVPreprocessParam*)parameters->bf_param;
  if (beamforming != NULL) {
    free(beamforming->dereverb_para);
    free(beamforming->aes_para);
    free(beamforming->anr_para);
    free(beamforming->agc_para);
    free(beamforming->dtd_para);
    free(beamforming->howl_para);
    free(beamforming);
  }
  SKVAECParameter* aec = (SKVAECParameter*)parameters->aec_param;
  if (aec != NULL) {
    free(aec->delay_para);
  }
  free(aec);
  parameters->aec_param = NULL;
  parameters->bf_param = NULL;
  rkaudio_fake_record("param_deinit");
}

#ifdef __cplusplus
extern "C" {
#endif
void* rkaudio_preprocess_init(
    int rate, int bits, int src_chan, int ref_chan, RKAUDIOParam* parameters);
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
            'extern "C" int rkaudio_common_anchor() { return 17; }\n',
            encoding="utf-8",
        )
        (source / "detect.cpp").write_text(
            'extern "C" int rkaudio_detect_anchor() { return 19; }\n',
            encoding="utf-8",
        )
        (source / "aec.cpp").write_text(
            """\
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "rkaudio_preprocess.h"

extern "C" int rkaudio_common_anchor();

namespace {

int g_handle = 23;

const char* fake_mode() {
  const char* mode = std::getenv("BOOMPI_3A_FAKE_MODE");
  return mode == nullptr ? "pass" : mode;
}

void record(const char* format, int first = 0, int second = 0,
            int third = 0, int fourth = 0) {
  const char* path = std::getenv("BOOMPI_3A_FAKE_SENTINEL");
  if (path == nullptr || path[0] == '\\0') {
    return;
  }
  if (std::FILE* output = std::fopen(path, "ab")) {
    std::fprintf(output, format, first, second, third, fourth);
    std::fputc('\\n', output);
    std::fclose(output);
  }
}

bool same_float(float actual, float expected) {
  return std::fabs(actual - expected) <= 0.000001F;
}

bool pinned_profile_matches(const RKAUDIOParam* parameters) {
  if (parameters == nullptr || parameters->aec_param == nullptr ||
      parameters->bf_param == nullptr) {
    return false;
  }
  const auto& g_aec_parameters =
      *static_cast<const SKVAECParameter*>(parameters->aec_param);
  const auto& g_bf_parameters =
      *static_cast<const SKVPreprocessParam*>(parameters->bf_param);
  if (g_aec_parameters.delay_para == nullptr ||
      g_bf_parameters.dereverb_para == nullptr ||
      g_bf_parameters.aes_para == nullptr ||
      g_bf_parameters.anr_para == nullptr ||
      g_bf_parameters.agc_para == nullptr ||
      g_bf_parameters.dtd_para == nullptr) {
    return false;
  }
  const auto& g_dereverb_parameters = *static_cast<const RKAudioDereverbParam*>(
      g_bf_parameters.dereverb_para);
  const auto& g_aes_parameters = *static_cast<const RKAudioAESParameter*>(
      g_bf_parameters.aes_para);
  const auto& g_anr_parameters =
      *static_cast<const SKVANRParam*>(g_bf_parameters.anr_para);
  const auto& g_agc_parameters =
      *static_cast<const RKAGCParam*>(g_bf_parameters.agc_para);
  const auto& g_dtd_parameters =
      *static_cast<const RKDTDParam*>(g_bf_parameters.dtd_para);
  static const float expected_limit_ratio[2][3] = {
      {2.0F, 1.5F, 1.0F}, {1.5F, 1.2F, 1.0F}};
  static const short expected_thd_split_freq[4][2] = {
      {0, 0}, {1500, 2000}, {2000, 6000}, {6000, 8000}};
  static const float expected_thd_sup_degree[4][10] = {
      {0.01F, 0.01F, 0.005F, 0.005F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
      {0.0005F, 0.0005F, 0.0005F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
      {0.001F, 0.001F, 0.001F, 0.001F, 0.001F, 0.001F, 0.0F, 0.0F, 0.0F, 0.0F},
      {0.001F, 0.001F, 0.001F, 0.001F, 0.001F, 0.001F, 0.0F, 0.0F, 0.0F, 0.0F}};
  static const short expected_hard_split_freq[5][2] = {
      {100, 500}, {0, 0}, {0, 0}, {0, 0}, {500, 3000}};
  static const float expected_hard_threshold[4] = {
      0.35F, 0.15F, 0.35F, 0.15F};

  return g_aec_parameters.pos == 1 &&
      g_aec_parameters.drop_ref_channel == 0 &&
      g_aec_parameters.model_aec_en == 0 &&
      g_aec_parameters.delay_len == 0 &&
      g_aec_parameters.look_ahead == 0 &&
      g_aec_parameters.filter_len == 2 &&
      g_bf_parameters.model_bf_en == 1141 &&
      g_bf_parameters.ref_pos == 1 && g_bf_parameters.Targ == 4 &&
      g_bf_parameters.num_ref_channel == 2 &&
      g_bf_parameters.drop_ref_channel == 0 &&
      same_float(g_dtd_parameters.ksiThd_high, 0.70F) &&
      same_float(g_dtd_parameters.ksiThd_low, 0.50F) &&
      g_dereverb_parameters.rlsLg == 4 &&
      g_dereverb_parameters.curveLg == 20 &&
      g_dereverb_parameters.delay == 2 &&
      same_float(g_dereverb_parameters.forgetting, 0.98F) &&
      same_float(g_dereverb_parameters.T60, 0.4F) &&
      same_float(g_dereverb_parameters.coCoeff, 1.0F) &&
      same_float(g_aes_parameters.Beta_Up, 0.002F) &&
      same_float(g_aes_parameters.Beta_Down, 0.001F) &&
      same_float(g_aes_parameters.Beta_Up_Low, 0.005F) &&
      same_float(g_aes_parameters.Beta_Down_Low, 0.001F) &&
      g_aes_parameters.low_freq == 500 &&
      g_aes_parameters.high_freq == 3750 &&
      g_aes_parameters.THD_Flag == 0 && g_aes_parameters.HARD_Flag == 0 &&
      std::memcmp(g_aes_parameters.LimitRatio, expected_limit_ratio,
                  sizeof(expected_limit_ratio)) == 0 &&
      std::memcmp(g_aes_parameters.ThdSplitFreq, expected_thd_split_freq,
                  sizeof(expected_thd_split_freq)) == 0 &&
      std::memcmp(g_aes_parameters.ThdSupDegree, expected_thd_sup_degree,
                  sizeof(expected_thd_sup_degree)) == 0 &&
      std::memcmp(g_aes_parameters.HardSplitFreq, expected_hard_split_freq,
                  sizeof(expected_hard_split_freq)) == 0 &&
      std::memcmp(g_aes_parameters.HardThreshold, expected_hard_threshold,
                  sizeof(expected_hard_threshold)) == 0 &&
      same_float(g_anr_parameters.noiseFactor, 0.88F) &&
      g_anr_parameters.swU == 1 && same_float(g_anr_parameters.PsiMin, 0.02F) &&
      same_float(g_anr_parameters.PsiMax, 0.516F) &&
      same_float(g_anr_parameters.fGmin, 0.01F) &&
      g_anr_parameters.Sup_Freq1 == -3588 &&
      g_anr_parameters.Sup_Freq2 == -3588 &&
      same_float(g_anr_parameters.Sup_Energy1, 10000.0F) &&
      same_float(g_anr_parameters.Sup_Energy2, 10000.0F) &&
      g_anr_parameters.InterV == 1 &&
      same_float(g_anr_parameters.BiasMin, 1.67F) &&
      g_anr_parameters.UpdateFrm == 15 &&
      same_float(g_anr_parameters.NPreGammaThr, 4.6F) &&
      same_float(g_anr_parameters.NPreZetaThr, 1.67F) &&
      same_float(g_anr_parameters.SabsGammaThr0, 1.0F) &&
      same_float(g_anr_parameters.SabsGammaThr1, 3.0F) &&
      same_float(g_anr_parameters.InfSmooth, 0.8F) &&
      same_float(g_anr_parameters.ProbSmooth, 0.7F) &&
      same_float(g_anr_parameters.CompCoeff, 1.4F) &&
      same_float(g_anr_parameters.PrioriMin, 0.0316F) &&
      same_float(g_anr_parameters.PostMax, 40.0F) &&
      same_float(g_anr_parameters.PrioriRatio, 0.95F) &&
      same_float(g_anr_parameters.PrioriRatioLow, 0.95F) &&
      g_anr_parameters.SplitBand == 20 &&
      same_float(g_anr_parameters.PrioriSmooth, 0.7F) &&
      g_anr_parameters.TranMode == 0 &&
      same_float(g_agc_parameters.attack_time, 200.0F) &&
      same_float(g_agc_parameters.release_time, 200.0F) &&
      same_float(g_agc_parameters.attenuate_time, 1000.0F) &&
      same_float(g_agc_parameters.max_gain, 25.0F) &&
      same_float(g_agc_parameters.max_peak, -1.0F) &&
      same_float(g_agc_parameters.fRth0, -55.0F) &&
      same_float(g_agc_parameters.fRth1, -45.0F) &&
      same_float(g_agc_parameters.fRth2, -30.0F) &&
      same_float(g_agc_parameters.fRk0, 2.0F) &&
      same_float(g_agc_parameters.fRk1, 0.8F) &&
      same_float(g_agc_parameters.fRk2, 0.4F) &&
      same_float(g_agc_parameters.fLineGainDb, -25.0F) &&
      g_agc_parameters.swSmL0 == 40 && g_agc_parameters.swSmL1 == 80 &&
      g_agc_parameters.swSmL2 == 80 && g_agc_parameters.fs == 16000 &&
      g_agc_parameters.frmlen == 256;
}

__attribute__((constructor)) void record_library_load() {
  record("library_load");
}

__attribute__((destructor)) void record_library_unload() {
  record("library_unload");
}

}  // namespace

extern "C" void* rkaudio_preprocess_init(
    int rate, int bits, int src_chan, int ref_chan,
    RKAUDIOParam* parameters) {
  (void)rkaudio_common_anchor();
  record("init rate=%d bits=%d src_chan=%d ref_chan=%d", rate, bits,
         src_chan, ref_chan);
  if (std::strcmp(fake_mode(), "init_null") == 0) {
    return nullptr;
  }
  if (parameters == nullptr || parameters->model_en != 3 ||
      parameters->read_size != 256 || parameters->rx_param != nullptr ||
      !pinned_profile_matches(parameters)) {
    return nullptr;
  }
  return &g_handle;
}

extern "C" int rkaudio_preprocess_short(
    void*, short*, short* output, int input_size, int* wakeup_status) {
  const bool mismatch = std::strcmp(fake_mode(), "process_mismatch") == 0;
  const int result = mismatch ? 511 : 512;
  record("process input_size=%d return=%d", input_size, result);
  if (output != nullptr) {
    for (int index = 0; index < 256; ++index) {
      output[index] = static_cast<short>(index - 128);
    }
  }
  if (wakeup_status != nullptr) {
    *wakeup_status = 37;
  }
  return result;
}

extern "C" void rkaudio_preprocess_destory(void*) {
  record("destroy");
}
""",
            encoding="utf-8",
        )
        (source / "incomplete.cpp").write_text(
            """\
#include <cstdio>
#include <cstdlib>

namespace {
void record(const char* message) {
  const char* path = std::getenv("BOOMPI_3A_FAKE_SENTINEL");
  if (path == nullptr || path[0] == '\\0') {
    return;
  }
  if (std::FILE* output = std::fopen(path, "ab")) {
    std::fprintf(output, "%s\\n", message);
    std::fclose(output);
  }
}

__attribute__((constructor)) void record_library_load() {
  record("library_load");
}

__attribute__((destructor)) void record_library_unload() {
  record("library_unload");
}
}  // namespace

extern "C" void* rkaudio_preprocess_init(int, int, int, int, void*) {
  record("unexpected_vendor_call");
  return nullptr;
}
""",
            encoding="utf-8",
        )
        (source / "CMakeLists.txt").write_text(
            """\
cmake_minimum_required(VERSION 3.21)
project(boompi_fake_rockchip_3a LANGUAGES CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
foreach(_config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set("CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_config}" "${CMAKE_BINARY_DIR}/out")
endforeach()

add_library(fake_common SHARED common.cpp)
set_target_properties(fake_common PROPERTIES
  OUTPUT_NAME rkaudio_common PREFIX "lib" SUFFIX ".so"
  SKIP_BUILD_RPATH TRUE)

add_library(fake_detect SHARED detect.cpp)
set_target_properties(fake_detect PROPERTIES
  OUTPUT_NAME rkaudio_detect PREFIX "lib" SUFFIX ".so"
  SKIP_BUILD_RPATH TRUE)

add_library(fake_aec SHARED aec.cpp)
target_include_directories(fake_aec PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fake_aec PRIVATE fake_common)
set_target_properties(fake_aec PROPERTIES
  OUTPUT_NAME aec_bf_process PREFIX "lib" SUFFIX ".so"
  BUILD_RPATH "\\$ORIGIN"
  INSTALL_RPATH "\\$ORIGIN")

add_library(fake_incomplete SHARED incomplete.cpp)
set_target_properties(fake_incomplete PROPERTIES
  OUTPUT_NAME aec_bf_process_incomplete PREFIX "lib" SUFFIX ".so"
  SKIP_BUILD_RPATH TRUE)
""",
            encoding="utf-8",
        )

        build = root / "fake-rockchip-3a-build"
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
            "common": output / "librkaudio_common.so",
            "detect": output / "librkaudio_detect.so",
            "aec": output / "libaec_bf_process.so",
            "incomplete": output / "libaec_bf_process_incomplete.so",
        }
        for logical_name, artifact in built_artifacts.items():
            self.assertTrue(artifact.is_file(), f"missing fake {logical_name}: {artifact}")

        private_root = root / PRIVATE_PATH_MARKER
        private_include = private_root / "include"
        private_include.mkdir(parents=True)
        header = private_include / "rkaudio_preprocess.h"
        shutil.copy2(include_dir / header.name, header)
        config = private_root / "config_aivqe.json"
        config.write_text("{}\n", encoding="utf-8")

        artifacts: dict[str, Path] = {
            "root": private_root,
            "header": header,
            "config": config,
        }
        for logical_name, artifact in built_artifacts.items():
            destination = private_root / artifact.name
            shutil.copy2(artifact, destination)
            artifacts[logical_name] = destination

        runtime_root = root / "fake-runtime"
        runtime_root.mkdir()
        runtime_aec = runtime_root / built_artifacts["aec"].name
        runtime_common = runtime_root / built_artifacts["common"].name
        runtime_incomplete = runtime_root / built_artifacts["incomplete"].name
        shutil.copy2(built_artifacts["aec"], runtime_aec)
        shutil.copy2(built_artifacts["common"], runtime_common)
        shutil.copy2(built_artifacts["incomplete"], runtime_incomplete)
        artifacts["runtime_aec"] = runtime_aec
        artifacts["runtime_incomplete"] = runtime_incomplete
        return artifacts

    def _build_production_adapter_runner(
        self, root: Path, artifacts: dict[str, Path]
    ) -> Path:
        source = root / "production-adapter-test"
        source.mkdir()
        fake_vendor = source / "production_fake_vendor.cpp"
        fake_vendor.write_text(
            """\
#include <cmath>
#include <cstddef>
#include <cstdio>

#include "rkaudio_preprocess.h"

static_assert(offsetof(RKAGCParam, release_time) ==
              offsetof(RKAGCParam, attack_time) + sizeof(float));
static_assert(offsetof(RKAGCParam, max_gain) ==
              offsetof(RKAGCParam, release_time) + sizeof(float));
static_assert(offsetof(RKAGCParam, max_peak) ==
              offsetof(RKAGCParam, max_gain) + sizeof(float));
static_assert(offsetof(RKAGCParam, fRth0) ==
              offsetof(RKAGCParam, max_peak) + sizeof(float));
static_assert(offsetof(RKAGCParam, fRk0) ==
              offsetof(RKAGCParam, fRth0) + sizeof(float));
static_assert(offsetof(RKAGCParam, fRth1) ==
              offsetof(RKAGCParam, fRk0) + sizeof(float));
static_assert(offsetof(RKAGCParam, fs) ==
              offsetof(RKAGCParam, fRth1) + sizeof(float));
static_assert(offsetof(RKAGCParam, frmlen) ==
              offsetof(RKAGCParam, fs) + sizeof(int));
static_assert(offsetof(RKAGCParam, attenuate_time) ==
              offsetof(RKAGCParam, frmlen) + sizeof(int));
static_assert(offsetof(RKAGCParam, fRth2) ==
              offsetof(RKAGCParam, attenuate_time) + sizeof(float));
static_assert(offsetof(RKAGCParam, fRk1) ==
              offsetof(RKAGCParam, fRth2) + sizeof(float));
static_assert(offsetof(RKAGCParam, fRk2) ==
              offsetof(RKAGCParam, fRk1) + sizeof(float));
static_assert(offsetof(RKAGCParam, fLineGainDb) ==
              offsetof(RKAGCParam, fRk2) + sizeof(float));
static_assert(offsetof(RKAGCParam, swSmL0) ==
              offsetof(RKAGCParam, fLineGainDb) + sizeof(float));
static_assert(offsetof(RKAGCParam, swSmL1) ==
              offsetof(RKAGCParam, swSmL0) + sizeof(int));
static_assert(offsetof(RKAGCParam, swSmL2) ==
              offsetof(RKAGCParam, swSmL1) + sizeof(int));

static_assert(offsetof(SKVPreprocessParam, aes_para) ==
              offsetof(SKVPreprocessParam, dereverb_para) + sizeof(void*));
static_assert(offsetof(SKVPreprocessParam, nlp_para) ==
              offsetof(SKVPreprocessParam, aes_para) + sizeof(void*));
static_assert(offsetof(SKVPreprocessParam, anr_para) ==
              offsetof(SKVPreprocessParam, nlp_para) + sizeof(void*));
static_assert(offsetof(SKVPreprocessParam, agc_para) ==
              offsetof(SKVPreprocessParam, anr_para) + sizeof(void*));
static_assert(offsetof(SKVPreprocessParam, cng_para) ==
              offsetof(SKVPreprocessParam, agc_para) + sizeof(void*));
static_assert(offsetof(SKVPreprocessParam, dtd_para) ==
              offsetof(SKVPreprocessParam, cng_para) + sizeof(void*));
static_assert(offsetof(SKVPreprocessParam, eq_para) ==
              offsetof(SKVPreprocessParam, dtd_para) + sizeof(void*));
static_assert(offsetof(SKVPreprocessParam, howl_para) ==
              offsetof(SKVPreprocessParam, eq_para) + sizeof(void*));
static_assert(offsetof(SKVPreprocessParam, doa_para) ==
              offsetof(SKVPreprocessParam, howl_para) + sizeof(void*));

namespace {

int g_handle = 41;
int g_init_calls = 0;
int g_process_calls = 0;
int g_destroy_calls = 0;
char g_error[192] = {};

void Fail(const char* message) {
  if (g_error[0] == '\\0') {
    std::snprintf(g_error, sizeof(g_error), "%s", message);
  }
}

bool SameFloat(float actual, float expected) {
  return std::fabs(actual - expected) <= 0.000001F;
}

}  // namespace

extern "C" void* rkaudio_preprocess_init(
    int rate, int bits, int src_chan, int ref_chan,
    RKAUDIOParam* parameters) {
  ++g_init_calls;
  if (rate != 16000 || bits != 16 || src_chan != 2 || ref_chan != 1) {
    Fail("init must be 16k/S16/src2/ref1");
    return nullptr;
  }
  if (parameters == nullptr || parameters->model_en != 3 ||
      parameters->read_size != 256 || parameters->rx_param != nullptr ||
      parameters->aec_param == nullptr || parameters->bf_param == nullptr) {
    Fail("invalid main Rockchip parameter tree");
    return nullptr;
  }
  const auto* aec =
      static_cast<const SKVAECParameter*>(parameters->aec_param);
  const auto* bf =
      static_cast<const SKVPreprocessParam*>(parameters->bf_param);
  if (aec->pos != 1 || aec->drop_ref_channel != 0 ||
      aec->model_aec_en != 0) {
    Fail("hardware reference must be ref-last without software delay");
    return nullptr;
  }
  if (bf->model_bf_en != 1109 || bf->ref_pos != 1 || bf->Targ != 4 ||
      bf->num_ref_channel != 1 || bf->drop_ref_channel != 0 ||
      bf->dtd_para == nullptr) {
    Fail("invalid 2+1 BF/STDT profile");
    return nullptr;
  }
  const auto* dtd = static_cast<const RKDTDParam*>(bf->dtd_para);
  if (!SameFloat(dtd->ksiThd_high, 0.70F) ||
      !SameFloat(dtd->ksiThd_low, 0.50F)) {
    Fail("invalid STDT thresholds");
    return nullptr;
  }
  return &g_handle;
}

extern "C" int rkaudio_preprocess_short(
    void* handle, short* input, short* output, int input_size,
    int* wakeup_status) {
  ++g_process_calls;
  if (handle != &g_handle || input == nullptr || output == nullptr ||
      wakeup_status == nullptr) {
    Fail("invalid process pointers");
    return 512;
  }
  if (input_size != 768) {
    Fail("input_size must be 768 shorts");
  }
  for (int sample = 0; sample < 256; ++sample) {
    for (int channel = 0; channel < 3; ++channel) {
      const short expected = static_cast<short>(
          (channel + 1) * 1000 + sample);
      const short actual = input[3 * sample + channel];
      if (actual != expected && g_error[0] == '\\0') {
        std::snprintf(g_error, sizeof(g_error),
                      "packing mismatch sample=%d channel=%d actual=%d expected=%d",
                      sample, channel, static_cast<int>(actual),
                      static_cast<int>(expected));
      }
    }
    output[sample] = static_cast<short>(sample - 128);
  }
  *wakeup_status = 0;
  return 512;
}

extern "C" void rkaudio_preprocess_destory(void* handle) {
  ++g_destroy_calls;
  if (handle != &g_handle) {
    Fail("destroy received the wrong handle");
  }
}

extern "C" int boompi_fake_contract_ok() {
  if (g_init_calls != 1 || g_process_calls != 1 || g_destroy_calls != 1) {
    Fail("unexpected production adapter call counts");
  }
  return g_error[0] == '\\0';
}

extern "C" const char* boompi_fake_contract_error() {
  return g_error;
}
""",
            encoding="utf-8",
        )

        runner = source / "production_adapter_runner.cpp"
        runner.write_text(
            """\
#include <algorithm>
#include <cstdio>

#include "boompi/platform/rv1106/rockchip_voice_dsp.h"

extern "C" int boompi_fake_contract_ok();
extern "C" const char* boompi_fake_contract_error();

int main() {
  using boompi::platform::rv1106::RockchipVoiceDsp;
  using boompi::platform::rv1106::RockchipVoiceDspStatus;
  using boompi::platform::rv1106::RockchipVoiceFrame16k;

  RockchipVoiceDsp dsp;
  if (dsp.Open() != RockchipVoiceDspStatus::kOk) {
    std::fprintf(stderr, "production adapter open failed: %s\\n",
                 boompi_fake_contract_error());
    return 1;
  }

  RockchipVoiceFrame16k mic_left{};
  RockchipVoiceFrame16k mic_right{};
  RockchipVoiceFrame16k reference_left{};
  RockchipVoiceFrame16k reference_right{};
  RockchipVoiceFrame16k output{};
  for (std::size_t sample = 0; sample < mic_left.size(); ++sample) {
    mic_left[sample] = static_cast<short>(1000 + sample);
    mic_right[sample] = static_cast<short>(2000 + sample);
    reference_left[sample] = static_cast<short>(3000 + sample);
    reference_right[sample] = static_cast<short>(4000 + sample);
  }

  const auto status = dsp.Process(mic_left, mic_right, reference_left,
                                  reference_right, &output);
  if (status != RockchipVoiceDspStatus::kOk) {
    std::fprintf(stderr, "production adapter process failed: %s\\n",
                 boompi_fake_contract_error());
    return 2;
  }
  if (!std::all_of(output.begin(), output.end(),
                   [](short sample) { return sample == 0; })) {
    std::fprintf(stderr, "production adapter prime frame was not silent\\n");
    return 3;
  }

  dsp.Close();
  if (!boompi_fake_contract_ok()) {
    std::fprintf(stderr, "production adapter contract failed: %s\\n",
                 boompi_fake_contract_error());
    return 4;
  }
  std::puts("production RockchipVoiceDsp 2+1 contract passed");
  return 0;
}
""",
            encoding="utf-8",
        )

        executable = source / "production_adapter_runner"
        compiled = subprocess.run(
            [
                str(self.host_cxx),
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Werror",
                "-I",
                str(PRODUCTION_INCLUDE_DIR),
                "-I",
                str(artifacts["header"].parent),
                str(PRODUCTION_DSP_SOURCE),
                str(fake_vendor),
                str(runner),
                "-o",
                str(executable),
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        return executable

    def _enabled_definitions(
        self,
        root: Path,
        artifacts: dict[str, Path],
    ) -> list[str]:
        return [
            "-DBOOMPI_ENABLE_ROCKCHIP_3A=ON",
            "-DBOOMPI_BUILD_ROCKCHIP_3A_HIL=ON",
            "-DBOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON",
            "-DBOOMPI_BUILD_TESTS=OFF",
            "-DBUILD_TESTING=ON",
            "-DCMAKE_BUILD_TYPE:STRING=Debug",
            "-DTEST_ENABLE_CTEST=ON",
            "-DTEST_EXPECT_LINK_TARGET=ON",
            "-DTEST_EXPECT_HIL_TARGET=ON",
            f"-DTEST_HIL_VENDOR_LIBRARY_PATH:FILEPATH={artifacts['runtime_aec']}",
            *self._write_target_compiler_wrapper(root),
            f"-DBOOMPI_ROCKCHIP_3A_INCLUDE_DIR:PATH={artifacts['header'].parent}",
            f"-DBOOMPI_ROCKCHIP_3A_AEC_LIBRARY:FILEPATH={artifacts['aec']}",
            f"-DBOOMPI_ROCKCHIP_3A_COMMON_LIBRARY:FILEPATH={artifacts['common']}",
            f"-DBOOMPI_ROCKCHIP_3A_DETECT_LIBRARY:FILEPATH={artifacts['detect']}",
            f"-DBOOMPI_ROCKCHIP_3A_CONFIG_FILE:FILEPATH={artifacts['config']}",
        ]

    def _executables(self, build: Path, target: str) -> list[Path]:
        names = {target, f"{target}.exe"}
        return [
            candidate
            for candidate in build.rglob(f"{target}*")
            if candidate.is_file() and candidate.name in names
        ]

    def _find_executable(self, build: Path, target: str) -> Path:
        candidates = self._executables(build, target)
        self.assertEqual(
            len(candidates), 1, f"{target} was not built exactly once"
        )
        return candidates[0]

    def _runtime_environment(
        self,
        artifacts: dict[str, Path],
        sentinel: Path,
        mode: str,
    ) -> dict[str, str]:
        environment = dict(environ)
        for variable in (*VENDOR_DEBUG_PATH_VARIABLES, *LOADER_OVERRIDE_VARIABLES):
            environment.pop(variable, None)
        environment[SENTINEL_ENV] = str(sentinel)
        environment[MODE_ENV] = mode
        return environment

    def _run_probe(
        self,
        executable: Path,
        environment: dict[str, str],
        *arguments: str,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(executable), *arguments],
            check=False,
            capture_output=True,
            text=True,
            env=environment,
            timeout=5,
        )

    def _sentinel_lines(self, sentinel: Path) -> list[str]:
        if not sentinel.exists():
            return []
        return sentinel.read_text(encoding="utf-8").splitlines()

    def _assert_failed_without_private_path(
        self, completed: subprocess.CompletedProcess[str]
    ) -> None:
        output = completed.stdout + completed.stderr
        self.assertNotEqual(completed.returncode, 0, output)
        self.assertNotIn(PRIVATE_PATH_MARKER, output)

    def _assert_gate_failure(
        self,
        case_name: str,
        definitions: tuple[str, ...],
        expected_fragments: tuple[str, ...],
    ) -> None:
        with tempfile.TemporaryDirectory(
            prefix="boompi-rockchip-3a-hil-"
        ) as temporary:
            root = Path(temporary)
            self._write_fixture_project(root)
            module = self._copy_module_and_sources(root, case_name)
            private = root / PRIVATE_PATH_MARKER
            configured = self._configure(
                root,
                case_name,
                module,
                *definitions,
                f"-DBOOMPI_ROCKCHIP_3A_INCLUDE_DIR:PATH={private / 'include'}",
                f"-DBOOMPI_ROCKCHIP_3A_AEC_LIBRARY:FILEPATH={private / 'aec.so'}",
                f"-DBOOMPI_ROCKCHIP_3A_COMMON_LIBRARY:FILEPATH={private / 'common.so'}",
                f"-DBOOMPI_ROCKCHIP_3A_DETECT_LIBRARY:FILEPATH={private / 'detect.so'}",
                f"-DBOOMPI_ROCKCHIP_3A_CONFIG_FILE:FILEPATH={private / 'config.json'}",
            )
        self._assert_failed_without_private_path(configured)
        output = configured.stdout + configured.stderr
        for expected in expected_fragments:
            self.assertIn(expected, output)

    def test_default_off_does_not_touch_inputs_or_create_targets(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-rockchip-3a-hil-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root)
            module = self._copy_module_and_sources(root, "default-off")
            private = root / PRIVATE_PATH_MARKER
            configured = self._configure(
                root,
                "default-off",
                module,
                f"-DBOOMPI_ROCKCHIP_3A_INCLUDE_DIR:PATH={private / 'include'}",
                f"-DBOOMPI_ROCKCHIP_3A_AEC_LIBRARY:FILEPATH={private / 'aec.so'}",
                f"-DBOOMPI_ROCKCHIP_3A_COMMON_LIBRARY:FILEPATH={private / 'common.so'}",
                f"-DBOOMPI_ROCKCHIP_3A_DETECT_LIBRARY:FILEPATH={private / 'detect.so'}",
                f"-DBOOMPI_ROCKCHIP_3A_CONFIG_FILE:FILEPATH={private / 'config.json'}",
                "-DTEST_EXPECT_NO_LINK_TARGET=ON",
                "-DTEST_EXPECT_NO_HIL_TARGET=ON",
            )

        output = configured.stdout + configured.stderr
        self.assertEqual(configured.returncode, 0, output)
        self.assertNotIn(PRIVATE_PATH_MARKER, output)

    def test_hil_requires_the_rockchip_3a_dependency(self) -> None:
        self._assert_gate_failure(
            "missing-dependency",
            ("-DBOOMPI_BUILD_ROCKCHIP_3A_HIL=ON",),
            ("BOOMPI_BUILD_ROCKCHIP_3A_HIL", "BOOMPI_ENABLE_ROCKCHIP_3A"),
        )

    def test_hil_rejects_a_native_host_before_private_inputs(self) -> None:
        self._assert_gate_failure(
            "native-host",
            (
                "-DBOOMPI_ENABLE_ROCKCHIP_3A=ON",
                "-DBOOMPI_BUILD_ROCKCHIP_3A_HIL=ON",
                "-DBOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON",
            ),
            ("BOOMPI_TARGET_RV1106",),
        )

    def test_hil_rejects_a_host_target_boolean_spoof(self) -> None:
        self._assert_gate_failure(
            "host-boolean-spoof",
            (
                "-DBOOMPI_TARGET_RV1106=ON",
                "-DCMAKE_SYSTEM_PROCESSOR:STRING=arm",
                "-DBOOMPI_ENABLE_ROCKCHIP_3A=ON",
                "-DBOOMPI_BUILD_ROCKCHIP_3A_HIL=ON",
                "-DBOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON",
            ),
            ("cross-compiled Linux/ARM",),
        )

    def test_direct_hil_constructor_repeats_the_host_gate(self) -> None:
        self._assert_gate_failure(
            "direct-host-boolean-spoof",
            (
                "-DTEST_CONFIGURE_HIL_DIRECT=ON",
                "-DBOOMPI_TARGET_RV1106=ON",
                "-DCMAKE_SYSTEM_PROCESSOR:STRING=arm",
                "-DBOOMPI_ENABLE_ROCKCHIP_3A=ON",
                "-DBOOMPI_BUILD_ROCKCHIP_3A_HIL=ON",
                "-DBOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON",
            ),
            ("cross-compiled Linux/ARM",),
        )

    def test_production_dsp_adapter_uses_mode1_two_plus_one_contract(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="boompi-rockchip-3a-production-adapter-"
        ) as temporary:
            root = Path(temporary)
            artifacts = self._prepare_fake_vendor(root)
            executable = self._build_production_adapter_runner(root, artifacts)
            completed = subprocess.run(
                [str(executable)],
                check=False,
                capture_output=True,
                text=True,
                timeout=5,
            )

        self.assertEqual(
            completed.returncode, 0, completed.stdout + completed.stderr
        )
        self.assertEqual(
            completed.stdout,
            "production RockchipVoiceDsp 2+1 contract passed\n",
        )
        self.assertEqual(completed.stderr, "")

    def test_opt_in_target_lifecycle_and_fake_vendor_contracts(self) -> None:
        with tempfile.TemporaryDirectory(prefix="boompi-rockchip-3a-hil-") as temporary:
            root = Path(temporary)
            self._write_fixture_project(root, languages="CXX")
            artifacts = self._prepare_fake_vendor(root)
            module = self._copy_module_and_sources(
                root, "enabled", pinned_inputs=artifacts
            )
            case_name = "enabled"
            sentinel = root / "vendor-calls.sentinel"
            build = root / f"build-{case_name}"
            environment = self._runtime_environment(artifacts, sentinel, "pass")

            configured = self._configure(
                root,
                case_name,
                module,
                *self._enabled_definitions(root, artifacts),
            )
            configure_output = configured.stdout + configured.stderr
            self.assertEqual(configured.returncode, 0, configure_output)
            self.assertNotIn(PRIVATE_PATH_MARKER, configure_output)
            self.assertFalse(sentinel.exists(), "configure invoked the fake vendor")

            target_help = self._build(
                root, case_name, target="help", environment=environment
            )
            self.assertEqual(
                target_help.returncode, 0, target_help.stdout + target_help.stderr
            )
            self.assertIn(HIL_TARGET, target_help.stdout + target_help.stderr)
            self.assertFalse(sentinel.exists(), "target discovery invoked the fake vendor")

            default_build = self._build(root, case_name, environment=environment)
            default_output = default_build.stdout + default_build.stderr
            self.assertEqual(default_build.returncode, 0, default_output)
            self._find_executable(build, LINK_CHECK_TARGET)
            self.assertEqual(
                self._executables(build, HIL_TARGET), [],
                "EXCLUDE_FROM_ALL HIL was produced by the default build",
            )
            self.assertFalse(sentinel.exists(), "default build invoked the fake vendor")

            explicit_build = self._build(
                root, case_name, target=HIL_TARGET, environment=environment
            )
            explicit_output = explicit_build.stdout + explicit_build.stderr
            self.assertEqual(explicit_build.returncode, 0, explicit_output)
            executable = self._find_executable(build, HIL_TARGET)
            self.assertFalse(sentinel.exists(), "explicit build invoked the fake vendor")

            listed_tests = self._ctest(root, case_name, "-N", environment=environment)
            self.assertEqual(
                listed_tests.returncode,
                0,
                listed_tests.stdout + listed_tests.stderr,
            )
            self.assertNotIn(HIL_TARGET, listed_tests.stdout + listed_tests.stderr)
            ran_tests = self._ctest(
                root, case_name, "--output-on-failure", environment=environment
            )
            self.assertEqual(ran_tests.returncode, 0, ran_tests.stdout + ran_tests.stderr)
            self.assertNotIn(HIL_TARGET, ran_tests.stdout + ran_tests.stderr)
            self.assertFalse(sentinel.exists(), "CTest invoked the fake vendor")

            install_prefix = root / "install-tree"
            installed = self._install(
                root, case_name, install_prefix, environment=environment
            )
            self.assertEqual(installed.returncode, 0, installed.stdout + installed.stderr)
            self.assertEqual(
                [
                    candidate
                    for candidate in install_prefix.rglob("*")
                    if candidate.is_file() and HIL_TARGET in candidate.name
                ],
                [],
            )
            manifest = build / "install_manifest.txt"
            if manifest.is_file():
                self.assertNotIn(HIL_TARGET, manifest.read_text(encoding="utf-8"))
            self.assertFalse(sentinel.exists(), "install invoked the fake vendor")

            readelf = shutil.which("readelf")
            if readelf is None:
                self.skipTest("readelf is required for HIL path-leak validation")
            dynamic = subprocess.run(
                [readelf, "-d", str(executable)],
                check=False,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(dynamic.returncode, 0, dynamic.stdout + dynamic.stderr)
            self.assertNotRegex(dynamic.stdout, r"\((?:RPATH|RUNPATH)\)")
            self.assertNotIn("libaec_bf_process.so", dynamic.stdout)
            self.assertNotIn("librkaudio_common.so", dynamic.stdout)
            self.assertNotIn(PRIVATE_PATH_MARKER, dynamic.stdout)
            self.assertNotIn(PRIVATE_PATH_MARKER.encode(), executable.read_bytes())

            fake_vendor_symbols = subprocess.run(
                [readelf, "--wide", "--dyn-syms", str(artifacts["aec"])],
                check=False,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(
                fake_vendor_symbols.returncode,
                0,
                fake_vendor_symbols.stdout + fake_vendor_symbols.stderr,
            )
            for helper_name in (
                "rkaudio_aec_param_init",
                "rkaudio_preprocess_param_init",
                "rkaudio_param_deinit",
            ):
                self.assertNotIn(helper_name, fake_vendor_symbols.stdout)
            for exported_name in (
                "rkaudio_preprocess_init",
                "rkaudio_preprocess_short",
                "rkaudio_preprocess_destory",
            ):
                self.assertIn(exported_name, fake_vendor_symbols.stdout)

            sentinel.unlink(missing_ok=True)
            dry_run = self._run_probe(executable, environment)
            self.assertEqual(dry_run.returncode, 0, dry_run.stdout + dry_run.stderr)
            dry_document = json.loads(dry_run.stdout)
            self.assertEqual(dry_document["mode"], "dry_run")
            self.assertFalse(dry_document["vendor_calls_attempted"])
            self.assertFalse(dry_document["vendor_library_load_attempted"])
            self.assertEqual(
                dry_document["mutated_external_state"], "not_evaluated"
            )
            self.assertEqual(dry_document["contract"]["rate_hz"], 16000)
            self.assertEqual(dry_document["contract"]["sample_bits"], 16)
            self.assertEqual(dry_document["contract"]["source_channels"], 2)
            self.assertEqual(dry_document["contract"]["reference_channels"], 2)
            self.assertEqual(dry_document["contract"]["input_size_argument_shorts"], 1024)
            self.assertEqual(dry_document["contract"]["expected_output_bytes"], 512)
            self.assertEqual(self._sentinel_lines(sentinel), [])
            self.assertNotIn(PRIVATE_PATH_MARKER, dry_run.stdout + dry_run.stderr)

            for variable in (
                *VENDOR_DEBUG_PATH_VARIABLES,
                "LD_LIBRARY_PATH",
                "LD_AUDIT",
            ):
                with self.subTest(unsafe_environment_variable=variable):
                    unsafe_environment = self._runtime_environment(
                        artifacts, sentinel, "pass"
                    )
                    unsafe_environment[variable] = "/tmp/forbidden-dump.raw"
                    sentinel.unlink(missing_ok=True)
                    unsafe = self._run_probe(
                        executable,
                        unsafe_environment,
                        "--execute",
                        "--allow-rockchip-3a-call",
                    )
                    self.assertEqual(
                        unsafe.returncode, 3, unsafe.stdout + unsafe.stderr
                    )
                    self.assertEqual(self._sentinel_lines(sentinel), [])

            preload_environment = self._runtime_environment(
                artifacts, sentinel, "pass"
            )
            preload_environment["LD_PRELOAD"] = str(
                artifacts["runtime_incomplete"]
            )
            sentinel.unlink(missing_ok=True)
            preloaded_dry_run = self._run_probe(executable, preload_environment)
            self.assertEqual(
                preloaded_dry_run.returncode,
                3,
                preloaded_dry_run.stdout + preloaded_dry_run.stderr,
            )
            self.assertEqual(preloaded_dry_run.stdout, "")
            self.assertEqual(
                self._sentinel_lines(sentinel),
                ["library_load", "library_unload"],
            )

            runtime_aec = artifacts["runtime_aec"]
            held_aec = runtime_aec.with_suffix(".so.held")
            runtime_aec.replace(held_aec)
            try:
                sentinel.unlink(missing_ok=True)
                missing_library = self._run_probe(
                    executable,
                    self._runtime_environment(artifacts, sentinel, "pass"),
                    "--execute",
                    "--allow-rockchip-3a-call",
                )
            finally:
                held_aec.replace(runtime_aec)
            self.assertEqual(
                missing_library.returncode,
                4,
                missing_library.stdout + missing_library.stderr,
            )
            missing_document = json.loads(missing_library.stdout)
            self.assertEqual(missing_document["probe_status"], "vendor_load_failed")
            self.assertFalse(missing_document["vendor_library"]["loaded"])
            self.assertFalse(
                missing_document["vendor_library"]["symbols_resolved"]
            )
            self.assertFalse(missing_document["vendor_library"]["unloaded"])
            self.assertEqual(self._sentinel_lines(sentinel), [])

            runtime_aec.replace(held_aec)
            shutil.copy2(artifacts["runtime_incomplete"], runtime_aec)
            try:
                sentinel.unlink(missing_ok=True)
                missing_symbol = self._run_probe(
                    executable,
                    self._runtime_environment(artifacts, sentinel, "pass"),
                    "--execute",
                    "--allow-rockchip-3a-call",
                )
            finally:
                runtime_aec.unlink(missing_ok=True)
                held_aec.replace(runtime_aec)
            self.assertEqual(
                missing_symbol.returncode,
                4,
                missing_symbol.stdout + missing_symbol.stderr,
            )
            missing_symbol_document = json.loads(missing_symbol.stdout)
            self.assertEqual(
                missing_symbol_document["probe_status"], "vendor_load_failed"
            )
            self.assertTrue(missing_symbol_document["vendor_library"]["loaded"])
            self.assertFalse(
                missing_symbol_document["vendor_library"]["symbols_resolved"]
            )
            self.assertTrue(
                missing_symbol_document["vendor_library"]["unloaded"]
            )
            self.assertEqual(
                self._sentinel_lines(sentinel),
                ["library_load", "library_unload"],
            )

            successful_environment = self._runtime_environment(
                artifacts, sentinel, "pass"
            )
            sentinel.unlink(missing_ok=True)
            successful = self._run_probe(
                executable,
                successful_environment,
                "--execute",
                "--allow-rockchip-3a-call",
            )
            self.assertEqual(
                successful.returncode, 0, successful.stdout + successful.stderr
            )
            successful_document = json.loads(successful.stdout)
            self.assertEqual(successful_document["probe_status"], "pass")
            self.assertTrue(successful_document["vendor_library"]["loaded"])
            self.assertTrue(
                successful_document["vendor_library"]["symbols_resolved"]
            )
            self.assertTrue(successful_document["vendor_library"]["unloaded"])
            self.assertTrue(successful_document["calls"]["init"]["attempted"])
            self.assertTrue(successful_document["calls"]["init"]["returned_handle"])
            self.assertTrue(successful_document["calls"]["process"]["attempted"])
            self.assertEqual(successful_document["calls"]["process"]["return_value"], 512)
            self.assertTrue(successful_document["calls"]["destroy"]["called"])
            self.assertTrue(successful_document["calls"]["parameter_deinit"]["called"])
            self.assertTrue(successful_document["guards"]["evaluated"])
            self.assertTrue(successful_document["guards"]["input_intact"])
            self.assertTrue(successful_document["guards"]["output_intact"])
            self.assertEqual(
                self._sentinel_lines(sentinel),
                [
                    "library_load",
                    "aec_param_init",
                    "bf_param_init",
                    "init rate=16000 bits=16 src_chan=2 ref_chan=2",
                    "process input_size=1024 return=512",
                    "destroy",
                    "param_deinit",
                    "library_unload",
                ],
            )

            init_null_environment = self._runtime_environment(
                artifacts, sentinel, "init_null"
            )
            sentinel.unlink(missing_ok=True)
            init_null = self._run_probe(
                executable,
                init_null_environment,
                "--execute",
                "--allow-rockchip-3a-call",
            )
            self.assertEqual(init_null.returncode, 4, init_null.stdout + init_null.stderr)
            init_null_document = json.loads(init_null.stdout)
            self.assertEqual(init_null_document["probe_status"], "init_failed")
            self.assertFalse(init_null_document["calls"]["init"]["returned_handle"])
            self.assertFalse(init_null_document["calls"]["process"]["attempted"])
            self.assertFalse(init_null_document["calls"]["destroy"]["called"])
            self.assertTrue(init_null_document["calls"]["parameter_deinit"]["called"])
            self.assertFalse(init_null_document["guards"]["evaluated"])
            self.assertFalse(init_null_document["guards"]["input_intact"])
            self.assertFalse(init_null_document["guards"]["output_intact"])
            self.assertEqual(
                self._sentinel_lines(sentinel),
                [
                    "library_load",
                    "aec_param_init",
                    "bf_param_init",
                    "init rate=16000 bits=16 src_chan=2 ref_chan=2",
                    "param_deinit",
                    "library_unload",
                ],
            )

            mismatch_environment = self._runtime_environment(
                artifacts, sentinel, "process_mismatch"
            )
            sentinel.unlink(missing_ok=True)
            mismatch = self._run_probe(
                executable,
                mismatch_environment,
                "--execute",
                "--allow-rockchip-3a-call",
            )
            self.assertEqual(mismatch.returncode, 5, mismatch.stdout + mismatch.stderr)
            mismatch_document = json.loads(mismatch.stdout)
            self.assertEqual(
                mismatch_document["probe_status"], "process_contract_failed"
            )
            self.assertEqual(mismatch_document["calls"]["process"]["return_value"], 511)
            self.assertTrue(mismatch_document["calls"]["destroy"]["called"])
            self.assertTrue(mismatch_document["calls"]["parameter_deinit"]["called"])
            self.assertTrue(mismatch_document["guards"]["evaluated"])
            self.assertTrue(mismatch_document["guards"]["input_intact"])
            self.assertTrue(mismatch_document["guards"]["output_intact"])
            self.assertEqual(
                self._sentinel_lines(sentinel),
                [
                    "library_load",
                    "aec_param_init",
                    "bf_param_init",
                    "init rate=16000 bits=16 src_chan=2 ref_chan=2",
                    "process input_size=1024 return=511",
                    "destroy",
                    "param_deinit",
                    "library_unload",
                ],
            )

            for completed in (successful, init_null, mismatch):
                self.assertNotIn(
                    PRIVATE_PATH_MARKER, completed.stdout + completed.stderr
                )


if __name__ == "__main__":
    unittest.main()
