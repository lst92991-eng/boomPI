import json
import re
import unittest
from pathlib import Path
from urllib.parse import unquote


ROOT = Path(__file__).resolve().parents[2]
# 2500 ELOC 只约束音频产品胶水；vendor ABI 适配单列，LVGL 和 NetworkBootstrap 独立
# 评审，不能让这些明确边界的正常增长误报为产品逻辑预算失败。
AUDIO_SOURCE_ROOTS = (
    ROOT / "client/apps/boompi_client",
    ROOT / "client/include/boompi/application",
    ROOT / "client/include/boompi/audio",
    ROOT / "client/include/boompi/config",
    ROOT / "client/include/boompi/network",
    ROOT / "client/include/boompi/platform/rv1106",
    ROOT / "client/src/application",
    ROOT / "client/src/audio",
    ROOT / "client/src/config",
    ROOT / "client/src/network",
    ROOT / "client/src/platform/rv1106",
)
AUDIO_EXCLUDED_FILES = {
    "client/include/boompi/network/network_bootstrap.h",
    "client/src/network/network_bootstrap.cpp",
}
AUDIO_MAINLINE_FILES = {
    "client/apps/boompi_client/main.cpp",
    "client/include/boompi/application/voice_client.h",
    "client/include/boompi/audio/audio_engine.h",
    "client/include/boompi/config/voice_client_config.h",
    "client/include/boompi/network/voice_transport.h",
    "client/include/boompi/platform/rv1106/rockchip_voice_dsp.h",
    "client/src/application/voice_client.cpp",
    "client/src/audio/audio_engine.cpp",
    "client/src/config/voice_client_config.cpp",
    "client/src/network/voice_transport.cpp",
    "client/src/platform/rv1106/audio_backend.cpp",
    "client/src/platform/rv1106/audio_backend.h",
    "client/src/platform/rv1106/rockchip_voice_dsp.cpp",
    "client/src/platform/rv1106/snowboy_legacy_bridge.cpp",
    "client/src/platform/rv1106/snowboy_legacy_bridge.h",
}
VENDOR_INTEGRATION = {
    "client/include/boompi/platform/rv1106/rockchip_voice_dsp.h",
    "client/src/platform/rv1106/rockchip_voice_dsp.cpp",
    "client/src/platform/rv1106/snowboy_legacy_bridge.cpp",
    "client/src/platform/rv1106/snowboy_legacy_bridge.h",
}
CURRENT_DOCUMENTS = (
    "README.md",
    "AGENTS.md",
    "docs/architecture/system-overview.md",
    "docs/architecture/audio-runtime.md",
    "docs/architecture/audio-backends.md",
    "docs/test/client-responsibility-layout-20260801.md",
)
METRIC_DOCUMENTS = (
    "README.md",
    "AGENTS.md",
    "docs/architecture/system-overview.md",
    "docs/architecture/audio-runtime.md",
)
RV1106_CANDIDATE_PATHS = {
    "BOOMPI_BOOST_INCLUDE_DIR",
    "BOOMPI_LVGL_ROOT",
    "BOOMPI_OPENBLAS_LIBRARY",
    "BOOMPI_OPENSSL_ROOT",
    "BOOMPI_ROCKCHIP_3A_AEC_LIBRARY",
    "BOOMPI_ROCKCHIP_3A_COMMON_LIBRARY",
    "BOOMPI_ROCKCHIP_3A_CONFIG_FILE",
    "BOOMPI_ROCKCHIP_3A_DETECT_LIBRARY",
    "BOOMPI_ROCKCHIP_3A_INCLUDE_DIR",
    "BOOMPI_SNOWBOY_INCLUDE_DIR",
    "BOOMPI_SNOWBOY_LIBRARY",
    "BOOMPI_SNOWBOY_MODEL_FILE",
    "BOOMPI_SNOWBOY_RESOURCE_FILE",
    "BOOMPI_WEBRTC_VAD_INCLUDE_DIR",
    "BOOMPI_WEBRTC_VAD_LIBRARY",
}
AUDIO_PRODUCT_ELOC_TARGET = 2500
AUDIO_PRODUCT_ELOC_LIMIT = 2800
AUDIO_VENDOR_ELOC_LIMIT = 300
AUDIO_MAINLINE_ELOC_LIMIT = 3100


def eloc(path: Path) -> int:
    """Keep the documented metric stable: nonblank lines except pure // comments."""
    return sum(
        1 for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.strip().startswith("//")
    )


class ClientSourceContractTest(unittest.TestCase):
    def test_audio_mainline_files_and_budget(self) -> None:
        suffixes = {".c", ".cc", ".cpp", ".h", ".hpp"}
        actual = {
            path.relative_to(ROOT).as_posix()
            for source_root in AUDIO_SOURCE_ROOTS
            for path in source_root.rglob("*")
            if path.is_file() and path.suffix in suffixes
        }
        self.assertTrue(
            all((ROOT / name).is_file() for name in AUDIO_EXCLUDED_FILES),
            "the independently reviewed network bootstrap boundary changed",
        )
        actual.difference_update(AUDIO_EXCLUDED_FILES)
        self.assertEqual(AUDIO_MAINLINE_FILES, actual)
        counts = {name: eloc(ROOT / name) for name in actual}
        vendor_integration = sum(counts[name] for name in VENDOR_INTEGRATION)
        product_core = sum(counts.values()) - vendor_integration
        # 2500 行是教学设计目标，不是压缩排版的理由。CI 给展开后的状态转换留出
        # 少量余量，同时分别约束产品胶水、vendor ABI 和完整音频主线。
        self.assertLessEqual(product_core, AUDIO_PRODUCT_ELOC_LIMIT)
        self.assertLessEqual(vendor_integration, AUDIO_VENDOR_ELOC_LIMIT)
        self.assertLessEqual(sum(counts.values()), AUDIO_MAINLINE_ELOC_LIMIT)

        for name in METRIC_DOCUMENTS:
            documented = (ROOT / name).read_text(encoding="utf-8")
            for expected in (str(sum(counts.values())), str(vendor_integration), str(product_core)):
                self.assertRegex(
                    documented,
                    rf"(?<!\d){re.escape(expected)}(?!\d)",
                    f"{name}: missing current ELOC metric {expected}",
                )

    def test_rv1106_candidate_preset_is_complete_and_path_free(self) -> None:
        document = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
        presets = {preset["name"]: preset for preset in document["configurePresets"]}
        self.assertEqual(
            {
                name
                for name, preset in presets.items()
                if name.startswith("rv1106-") and not preset.get("hidden", False)
            },
            {"rv1106-candidate"},
        )
        self.assertEqual(
            presets["host-base"]["cacheVariables"]["BOOMPI_BUILD_UI_SIMULATOR"],
            "OFF",
        )
        candidate = presets["rv1106-candidate"]
        cache = candidate["cacheVariables"]

        self.assertEqual(candidate["inherits"], "rv1106-base")
        self.assertEqual(cache["CMAKE_BUILD_TYPE"], "Debug")
        self.assertEqual(cache["CMAKE_C_FLAGS_DEBUG"], "-O2 -g")
        self.assertEqual(cache["CMAKE_CXX_FLAGS_DEBUG"], "-O2 -g")
        for option in (
            "BOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS",
            "BOOMPI_ENABLE_ALSA_PLAYBACK",
            "BOOMPI_ENABLE_ROCKCHIP_3A",
            "BOOMPI_ENABLE_SNOWBOY",
            "BOOMPI_ENABLE_WEBRTC_VAD",
            "BOOMPI_ENABLE_WSS_CLIENT",
            "BOOMPI_STRICT_WARNINGS",
        ):
            self.assertEqual(cache[option], "ON", f"{option} is not enabled")
        for variable in RV1106_CANDIDATE_PATHS:
            self.assertEqual(cache[variable], f"$env{{{variable}}}")
        for value in cache.values():
            if isinstance(value, str):
                self.assertFalse(value.startswith(("/", "\\")))
                self.assertFalse(re.match(r"^[A-Za-z]:[/\\]", value))

        toolchain = (
            ROOT / "client/cmake/toolchains/rv1106.cmake"
        ).read_text(encoding="utf-8")
        self.assertIn("ENV{BOOMPI_RV1106_TOOLCHAIN_ROOT}", toolchain)
        self.assertIn("ENV{BOOMPI_RV1106_SYSROOT}", toolchain)

        build_presets = {
            preset["name"]: preset for preset in document["buildPresets"]
        }
        self.assertEqual(
            {name for name in build_presets if name.startswith("rv1106-")},
            {"rv1106-candidate"},
        )
        self.assertEqual(
            build_presets["rv1106-candidate"]["configurePreset"],
            "rv1106-candidate",
        )

    def test_current_documents_have_no_broken_local_links(self) -> None:
        link_pattern = re.compile(r"\[[^]]*]\(([^)]+)\)")
        for name in CURRENT_DOCUMENTS:
            document = ROOT / name
            for raw_target in link_pattern.findall(document.read_text(encoding="utf-8")):
                target = unquote(raw_target.split("#", 1)[0].strip().strip("<>"))
                if not target or "://" in target or target.startswith("mailto:"):
                    continue
                resolved = (document.parent / target).resolve()
                self.assertTrue(resolved.exists(), f"{name}: missing local link {raw_target}")


if __name__ == "__main__":
    unittest.main()
