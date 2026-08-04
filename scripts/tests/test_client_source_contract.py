import re
import unittest
from pathlib import Path
from urllib.parse import unquote


ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOTS = (
    ROOT / "client/apps/boompi_client",
    ROOT / "client/include/boompi",
    ROOT / "client/src",
)
PRODUCTION_FILES = {
    "client/apps/boompi_client/main.cpp",
    "client/include/boompi/application/voice_client.h",
    "client/include/boompi/audio/audio_engine.h",
    "client/include/boompi/config/voice_client_config.h",
    "client/include/boompi/network/network_bootstrap.h",
    "client/include/boompi/network/voice_transport.h",
    "client/include/boompi/platform/rv1106/rockchip_voice_dsp.h",
    "client/include/boompi/ui/device_ui.h",
    "client/src/application/voice_client.cpp",
    "client/src/audio/audio_engine.cpp",
    "client/src/config/voice_client_config.cpp",
    "client/src/network/network_bootstrap.cpp",
    "client/src/network/voice_transport.cpp",
    "client/src/platform/rv1106/audio_backend.cpp",
    "client/src/platform/rv1106/audio_backend.h",
    "client/src/platform/rv1106/rockchip_voice_dsp.cpp",
    "client/src/platform/rv1106/snowboy_legacy_bridge.cpp",
    "client/src/platform/rv1106/snowboy_legacy_bridge.h",
    "client/src/ui/device_ui.cpp",
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


def eloc(path: Path) -> int:
    """Keep the documented metric stable: nonblank lines except pure // comments."""
    return sum(
        1 for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.strip().startswith("//")
    )


class ClientSourceContractTest(unittest.TestCase):
    def test_current_responsibility_files_and_budgets(self) -> None:
        suffixes = {".c", ".cc", ".cpp", ".h", ".hpp"}
        actual = {
            path.relative_to(ROOT).as_posix()
            for source_root in SOURCE_ROOTS
            for path in source_root.rglob("*")
            if path.is_file() and path.suffix in suffixes
        }
        self.assertEqual(PRODUCTION_FILES, actual)
        counts = {name: eloc(ROOT / name) for name in actual}
        vendor_integration = sum(counts[name] for name in VENDOR_INTEGRATION)
        product_core = sum(counts.values()) - vendor_integration
        self.assertLessEqual(sum(counts.values()), 2500)

        for name in METRIC_DOCUMENTS:
            documented = (ROOT / name).read_text(encoding="utf-8")
            for expected in (str(sum(counts.values())), str(vendor_integration), str(product_core)):
                self.assertIn(expected, documented, f"{name}: missing current ELOC metric {expected}")
            for stale in ("2300", "439", "1861"):
                self.assertNotIn(stale, documented, f"{name}: stale ELOC metric {stale}")

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
