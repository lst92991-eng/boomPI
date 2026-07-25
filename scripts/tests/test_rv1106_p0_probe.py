#!/usr/bin/env python3
"""Offline tests for the read-only RV1106 P0 probe."""

import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
PROBE = REPOSITORY_ROOT / "scripts" / "probes" / "rv1106_p0_probe.sh"


def write_text(root: Path, relative: str, text: str = "") -> Path:
    target = root / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding="utf-8")
    return target


def write_command(bin_dir: Path, name: str, body: str) -> None:
    target = write_text(bin_dir, name, "#!/bin/sh\n" + body)
    target.chmod(target.stat().st_mode | stat.S_IXUSR)


class P0ProbeTest(unittest.TestCase):
    maxDiff = None

    def run_probe(self, root: Path, bin_dir: Path, *arguments: str):
        environment = os.environ.copy()
        environment["BOOMPI_PROBE_ROOT"] = str(root)
        environment["PATH"] = os.pathsep.join((str(bin_dir), environment["PATH"]))
        completed = subprocess.run(
            ["/bin/sh", str(PROBE), *arguments],
            check=True,
            capture_output=True,
            text=True,
            env=environment,
        )
        return completed.stdout, json.loads(completed.stdout)

    def make_rich_fixture(self, base: Path):
        root = base / "private-user-home" / "fixture-root"
        bin_dir = base / "bin"
        bin_dir.mkdir(parents=True)

        write_text(root, "bin/sh", "ELF fixture")
        write_text(root, "proc/cpuinfo", "Features : half thumb fastmult vfp edsp neon vfpv3\n")
        write_text(root, "proc/asound/cards", " 0 [rockchipcodec ]: test - private card\n")
        write_text(
            root,
            "proc/asound/pcm",
            "00-00: test : playback 1 : capture 1\n",
        )
        write_text(root, "proc/bus/input/devices", "N: Name=\"goodix-gt911\"\n")
        write_text(root, "sys/bus/spi/devices/spi0.0/modalias", "spi:st7789v\n")

        write_text(root, "sys/class/net/wlxdeadbeefcafe/type", "1\n")
        write_text(root, "sys/class/net/wlxdeadbeefcafe/operstate", "up\n")
        (root / "sys/class/net/wlxdeadbeefcafe/wireless").mkdir(parents=True)
        write_text(root, "sys/class/net/enx112233445566/type", "1\n")
        write_text(root, "sys/class/net/enx112233445566/operstate", "up\n")
        write_text(root, "sys/class/net/lo/type", "772\n")
        write_text(root, "sys/class/net/lo/operstate", "unknown\n")

        for relative in (
            "dev/snd/pcmC0D0c",
            "dev/snd/pcmC0D0p",
            "dev/snd/controlC0",
            "dev/fb0",
            "dev/dri/card0",
            "dev/input/event0",
            "dev/video0",
            "dev/i2c-0",
            "dev/spidev0.0",
            "dev/watchdog",
            "oem/usr/lib/libaec_bf_process.so",
            "oem/usr/lib/libstdc++.so.6",
            "oem/usr/include/rkaudio_preprocess.h",
            "userdata/boompi/models/snowboy/libsnowboy-detect.a",
            "userdata/boompi/models/snowboy/SECRET_NETWORK.pmdl",
        ):
            write_text(root, relative, "fixture")

        write_command(
            bin_dir,
            "uname",
            'case "$1" in -s) echo Linux;; -m) echo armv7l;; -r) '
            'echo 5.10.110-private-host;; *) echo SECRET_HOSTNAME;; esac\n',
        )
        write_command(
            bin_dir,
            "getconf",
            'case "$1" in LONG_BIT) echo 32;; GNU_LIBC_VERSION) echo "glibc 2.36";; '
            '*) exit 1;; esac\n',
        )
        write_command(
            bin_dir,
            "readelf",
            'case "$1" in\n'
            '  -h) echo "Class: ELF32"; echo "Data: little endian"; '
            'echo "Machine: ARM"; echo "Flags: hard-float ABI";;\n'
            '  -A) echo "Tag_ABI_VFP_args: VFP registers";;\n'
            '  -l) echo "[Requesting program interpreter: /lib/ld-linux-armhf.so.3]";;\n'
            '  -d) echo "(NEEDED) Shared library: [libc.so.6]"; '
            'echo "private path: $2";;\n'
            '  *) exit 1;;\n'
            'esac\n',
        )
        write_command(bin_dir, "ldd", 'echo "ldd (GNU libc) 2.36"\n')
        write_command(
            bin_dir,
            "strings",
            'echo GLIBCXX_3.4.9; echo GLIBCXX_3.4.30; echo SECRET_TOKEN\n',
        )
        write_command(
            bin_dir,
            "iw",
            'echo "Supported interface modes:"; echo " * managed"; echo " * AP"; '
            'echo "SSID SECRET_NETWORK"; echo "addr de:ad:be:ef:00:01"; '
            'echo "192.168.7.2"\n',
        )
        for command in (
            "file",
            "aplay",
            "arecord",
            "amixer",
            "hostapd",
            "wpa_supplicant",
            "udhcpc",
            "fbset",
            "modetest",
        ):
            write_command(bin_dir, command, "exit 0\n")
        return root, bin_dir

    def test_rich_fixture_is_structured_and_redacted(self):
        with tempfile.TemporaryDirectory(prefix="boompi-probe-") as temporary:
            base = Path(temporary)
            root, bin_dir = self.make_rich_fixture(base)
            output, document = self.run_probe(root, bin_dir)

            self.assertEqual(document["schema_version"], 1)
            self.assertEqual(document["mode"], "read_only")
            self.assertEqual(document["target"]["machine"], "arm32")
            self.assertEqual(document["target"]["word_bits"], 32)
            self.assertEqual(document["target"]["endianness"], "little")
            self.assertEqual(document["target"]["float_abi"], "hard")
            self.assertEqual(document["target"]["libc"]["family"], "glibc")
            self.assertEqual(document["target"]["libstdcxx"]["max_glibcxx"], "GLIBCXX_3.4.30")

            self.assertEqual(document["alsa"]["card_count"], 1)
            self.assertTrue(document["alsa"]["full_duplex_candidate"])
            self.assertEqual(document["alsa"]["rate_48000_verified"], "unknown")
            self.assertEqual(document["vendor"]["rockchip_3a"]["status"], "candidate")
            self.assertEqual(document["vendor"]["rockchip_3a"]["target_abi_compatibility"], "candidate")
            self.assertEqual(document["vendor"]["snowboy"]["status"], "candidate")
            self.assertTrue(document["vendor"]["snowboy"]["model_present"])
            self.assertTrue(document["devices"]["gt911_detected"])
            self.assertTrue(document["devices"]["st7789_detected"])
            self.assertEqual(document["network"]["wifi"]["ap_mode"], "listed")
            self.assertEqual(document["network"]["wifi"]["sta_mode"], "listed")

            forbidden = (
                str(base),
                "private-user-home",
                "wlxdeadbeefcafe",
                "enx112233445566",
                "SECRET_NETWORK",
                "SECRET_TOKEN",
                "SECRET_HOSTNAME",
                "de:ad:be:ef:00:01",
                "192.168.7.2",
                "/lib/ld-linux-armhf.so.3",
            )
            for secret in forbidden:
                self.assertNotIn(secret, output)

    def test_empty_root_returns_valid_unknowns(self):
        with tempfile.TemporaryDirectory(prefix="boompi-probe-empty-") as temporary:
            base = Path(temporary)
            root = base / "empty-root"
            bin_dir = base / "bin"
            root.mkdir()
            bin_dir.mkdir()
            output, document = self.run_probe(root, bin_dir)

            self.assertTrue(output.startswith("{\n"))
            self.assertEqual(document["alsa"]["status"], "missing")
            self.assertEqual(document["vendor"]["rockchip_3a"]["status"], "missing")
            self.assertEqual(document["vendor"]["snowboy"]["status"], "missing")
            self.assertFalse(document["validation"]["alsa_devices_opened"])
            self.assertFalse(document["validation"]["wifi_scan_performed"])

    def test_optional_dependencies_can_be_missing(self):
        with tempfile.TemporaryDirectory(prefix="boompi-probe-minimal-") as temporary:
            base = Path(temporary)
            root = base / "empty-root"
            bin_dir = base / "minimal-bin"
            root.mkdir()
            bin_dir.mkdir()
            for command in ("cat", "date"):
                source = shutil.which(command)
                self.assertIsNotNone(source)
                os.symlink(source, bin_dir / command)

            environment = os.environ.copy()
            environment["BOOMPI_PROBE_ROOT"] = str(root)
            environment["PATH"] = str(bin_dir)
            completed = subprocess.run(
                ["/bin/sh", str(PROBE)],
                check=True,
                capture_output=True,
                text=True,
                env=environment,
            )
            document = json.loads(completed.stdout)
            self.assertEqual(document["tools"]["readelf"], "missing")
            self.assertEqual(document["tools"]["iw"], "missing")
            self.assertEqual(document["target"]["machine"], "unknown")
            self.assertEqual(document["target"]["libc"]["status"], "unknown")

    def test_help_does_not_require_target_dependencies(self):
        completed = subprocess.run(
            ["/bin/sh", str(PROBE), "--help"],
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertIn("read-only", completed.stdout)
        self.assertIn("nor emitted", completed.stdout)


if __name__ == "__main__":
    unittest.main()
