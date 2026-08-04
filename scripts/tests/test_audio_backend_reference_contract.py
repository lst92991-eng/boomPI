#!/usr/bin/env python3
"""Lock the production AEC path to dual-mic, single-reference."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).parents[2]
BACKEND = ROOT / "client/src/platform/rv1106/audio_backend.cpp"
DSP_HEADER = ROOT / "client/include/boompi/platform/rv1106/rockchip_voice_dsp.h"


class AudioBackendReferenceContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.backend = BACKEND.read_text(encoding="utf-8")
        cls.dsp_header = DSP_HEADER.read_text(encoding="utf-8")

    def test_reference_activity_uses_only_ref_left(self) -> None:
        assignment = re.search(
            r"const bool current_reference_active\s*=\s*([^;]+);",
            self.backend,
        )
        self.assertIsNotNone(assignment, "missing current_reference_active")
        expression = assignment.group(1)
        self.assertIn("ReferenceActive(impl_->reference_left)", expression)
        self.assertNotIn("reference_right", expression)

    def test_raw_capture_still_preserves_ref_right(self) -> None:
        self.assertIn(
            "impl_->reference_right[i] = impl_->capture16[base + 3U]",
            self.backend,
        )

    def test_production_dsp_defaults_to_one_reference(self) -> None:
        self.assertRegex(
            self.dsp_header,
            r"#define\s+BOOMPI_ROCKCHIP_REFERENCE_CHANNELS\s+1\b",
        )

    def test_playback_xrun_keeps_the_accepted_prefix_consumed(self) -> None:
        write_playback = re.search(
            r"bool WritePlayback\(.*?\n  }\n  bool UpdateVad",
            self.backend,
            re.DOTALL,
        )
        self.assertIsNotNone(write_playback, "missing WritePlayback implementation")
        body = write_playback.group(0)
        self.assertIn("playback_stereo.data() + 2U * offset", body)
        recovery = body.split("snd_pcm_recover", 1)[1].split("continue;", 1)[0]
        self.assertNotIn(
            "offset =",
            recovery,
            "playback xrun recovery replays the prefix already accepted by ALSA",
        )


if __name__ == "__main__":
    unittest.main()
