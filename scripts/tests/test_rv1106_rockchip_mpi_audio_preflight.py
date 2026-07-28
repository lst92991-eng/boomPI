#!/usr/bin/env python3
"""Offline contract tests for the read-only Rockchip MPI audio preflight."""

from __future__ import annotations

import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
PREFLIGHT = (
    REPOSITORY_ROOT
    / "scripts"
    / "probes"
    / "rv1106_rockchip_mpi_audio_preflight.sh"
)

TOOL_NAMES = {
    "dmesg",
    "readlink",
    "awk",
    "sleep",
    "usleep",
    "kill",
    "mkdir",
    "mv",
    "cmp",
    "sed",
    "wc",
    "sha256sum",
    "flock",
    "ps",
    "fuser",
    "scp",
    "timeout",
    "stat",
}
VALIDATION_FIELDS = {
    "files_created",
    "files_deleted",
    "files_renamed",
    "pcm_devices_opened",
    "mixer_accessed",
    "mpi_api_called",
    "signals_sent",
    "services_stopped",
    "processes_terminated",
    "network_identifiers_collected",
    "command_lines_collected",
    "environment_collected",
    "paths_emitted",
}
FORBIDDEN_COMMANDS = {
    "amixer",
    "arecord",
    "aplay",
    "kill",
    "killall",
    "mkdir",
    "mv",
    "rm",
}
BASE_REASON_CODES = [
    "exclusive_audio_service_control_unproven",
    "continuous_kernel_log_evidence_unproven",
    "service_control_automation_disabled",
]

SHELL_WRAPPER = r"""
amixer() { boompi-log-call amixer "$@"; return 97; }
arecord() { boompi-log-call arecord "$@"; return 97; }
aplay() { boompi-log-call aplay "$@"; return 97; }
kill() { boompi-log-call kill "$@"; return 97; }
killall() { boompi-log-call killall "$@"; return 97; }
mkdir() { boompi-log-call mkdir "$@"; return 97; }
mv() { boompi-log-call mv "$@"; return 97; }
rm() { boompi-log-call rm "$@"; return 97; }
probe_path=$1
shift
. "$probe_path"
"""

FAKE_COMMAND = r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys


def record(command, arguments):
    with open(os.environ["BOOMPI_PREFLIGHT_TEST_CALL_LOG"], "a", encoding="utf-8") as output:
        output.write(json.dumps(
            {"command": command, "argv": list(arguments)},
            ensure_ascii=True,
            separators=(",", ":"),
        ))
        output.write("\n")


invoked_as = Path(sys.argv[0]).name
if invoked_as == "boompi-log-call":
    if len(sys.argv) < 2:
        raise SystemExit(64)
    record(sys.argv[1], sys.argv[2:])
    raise SystemExit(0)

record(invoked_as, sys.argv[1:])

if invoked_as == "busybox":
    print("BusyBox v1.36.1 (boompi test fixture) multi-call binary.")
    raise SystemExit(0)

if invoked_as == "dmesg":
    if "--help" in sys.argv[1:] or "-h" in sys.argv[1:]:
        sys.stdout.write(os.environ.get(
            "BOOMPI_PREFLIGHT_TEST_DMESG_HELP",
            "Usage: dmesg [options]\\n  -w, --follow  wait for new messages\\n",
        ))
        raise SystemExit(0)
    if os.environ.get("BOOMPI_PREFLIGHT_TEST_DMESG_READABLE", "1") == "1":
        sys.stdout.write(os.environ.get(
            "BOOMPI_PREFLIGHT_TEST_DMESG_TEXT",
            "[    1.000000] fixture kernel message\\n",
        ))
        raise SystemExit(0)
    print("dmesg: read kernel buffer failed", file=sys.stderr)
    raise SystemExit(1)

if invoked_as == "readlink":
    if len(sys.argv) != 2:
        raise SystemExit(64)
    failure_suffix = os.environ.get(
        "BOOMPI_PREFLIGHT_TEST_READLINK_FAILURE_SUFFIX", ""
    )
    if failure_suffix and sys.argv[1].endswith(failure_suffix):
        raise SystemExit(37)
    try:
        print(os.readlink(sys.argv[1]))
    except OSError as error:
        print(f"readlink: {error}", file=sys.stderr)
        raise SystemExit(1)
    raise SystemExit(0)

# These commands are installed so command -v observes a deterministic target
# fixture. The preflight must not execute any of them.
raise SystemExit(97)
'''


def write_text(root: Path, relative: str, contents: str = "") -> Path:
    target = root / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(contents, encoding="utf-8")
    return target


def write_bytes(root: Path, relative: str, contents: bytes) -> Path:
    target = root / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(contents)
    return target


def write_symlink(root: Path, relative: str, target: str) -> Path:
    link = root / relative
    link.parent.mkdir(parents=True, exist_ok=True)
    link.symlink_to(target)
    return link


def tree_snapshot(root: Path) -> dict[str, tuple]:
    snapshot: dict[str, tuple] = {}
    for target in sorted(root.rglob("*"), key=lambda path: str(path)):
        relative = str(target.relative_to(root))
        mode = stat.S_IMODE(target.lstat().st_mode)
        if target.is_symlink():
            snapshot[relative] = ("symlink", mode, os.readlink(target))
        elif target.is_file():
            snapshot[relative] = ("file", mode, target.read_bytes())
        else:
            snapshot[relative] = ("directory", mode)
    return snapshot


class FakePreflightEnvironment:
    """A Linux filesystem fixture with argv-preserving fake commands."""

    def __init__(self, base: Path):
        self.base = base
        self.root = base / "private-user-home" / "fixture-root"
        self.bin_dir = base / "fake-bin"
        self.state_dir = base / "fake-state"
        self.call_log = self.state_dir / "calls.jsonl"
        self.root.mkdir(parents=True)
        self.bin_dir.mkdir()
        self.state_dir.mkdir()
        self.call_log.write_text("", encoding="utf-8")
        self._install_fake_commands()
        self._install_base_root()

    def _install_fake_commands(self) -> None:
        names = TOOL_NAMES | FORBIDDEN_COMMANDS | {"busybox", "boompi-log-call"}
        for name in names:
            command = self.bin_dir / name
            command.write_text(FAKE_COMMAND, encoding="utf-8")
            command.chmod(command.stat().st_mode | stat.S_IXUSR)

    def _install_base_root(self) -> None:
        write_text(self.root, "proc/1/comm", "init\n")
        write_symlink(self.root, "proc/1/exe", "/bin/busybox")
        (self.root / "proc/1/fd").mkdir(parents=True)
        write_text(self.root, "dev/kmsg", "private kernel stream fixture\n")
        write_text(self.root, "dev/mpi/vsys", "fixture node\n")
        write_text(self.root, "dev/snd/pcmC0D0c", "fixture node\n")

    def add_process(
        self,
        pid: int,
        name: str,
        fd_targets: tuple[str, ...] = (),
    ) -> None:
        process = self.root / "proc" / str(pid)
        write_text(process, "comm", f"{name}\n")
        fd_directory = process / "fd"
        fd_directory.mkdir(parents=True)
        for index, target in enumerate(fd_targets, start=3):
            (fd_directory / str(index)).symlink_to(target)

    def environment(self, **overrides: str) -> dict[str, str]:
        environment = os.environ.copy()
        environment.update(
            {
                "BOOMPI_HIL_PREFLIGHT_ROOT": str(self.root),
                "BOOMPI_PREFLIGHT_TEST_CALL_LOG": str(self.call_log),
                "BOOMPI_PREFLIGHT_TEST_DMESG_READABLE": "1",
                "BOOMPI_PREFLIGHT_TEST_DMESG_HELP": (
                    "Usage: dmesg [options]\n"
                    "  -w, --follow  wait for new messages\n"
                ),
                "LC_ALL": "C",
                "PATH": os.pathsep.join(
                    (str(self.bin_dir), environment.get("PATH", ""))
                ),
                "PYTHONDONTWRITEBYTECODE": "1",
            }
        )
        environment.update(overrides)
        return environment

    def run(self, *arguments: str, **environment_overrides: str):
        return subprocess.run(
            [
                "/bin/sh",
                "-c",
                SHELL_WRAPPER,
                "boompi-preflight-test",
                str(PREFLIGHT),
                *arguments,
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
            cwd=self.base,
            env=self.environment(**environment_overrides),
        )

    def calls(self, command: str | None = None) -> list[dict]:
        calls = [
            json.loads(line)
            for line in self.call_log.read_text(encoding="utf-8").splitlines()
            if line
        ]
        if command is None:
            return calls
        return [call for call in calls if call["command"] == command]


class RockchipMpiAudioPreflightTest(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls) -> None:
        if os.name != "posix" or not Path("/bin/sh").is_file():
            raise unittest.SkipTest("the preflight contract tests require Linux /bin/sh")
        if not PREFLIGHT.is_file():
            raise AssertionError(f"preflight script is missing: {PREFLIGHT}")

    def parse_report(self, completed: subprocess.CompletedProcess[str]) -> dict:
        self.assertEqual(
            completed.returncode,
            0,
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
        )
        return json.loads(completed.stdout)

    def assert_common_contract(self, document: dict) -> None:
        self.assertEqual(document["schema_version"], 1)
        self.assertEqual(
            document["probe"],
            "boompi-rv1106-rockchip-mpi-audio-preflight",
        )
        self.assertEqual(document["mode"], "read_only")
        self.assertIn(document["probe_status"], {"complete", "incomplete"})
        self.assertEqual(set(document["tools"]), TOOL_NAMES)
        self.assertTrue(
            all(value in {"available", "missing"} for value in document["tools"].values())
        )

        audio = document["audio"]
        for field in (
            "snd_fd_count",
            "snd_owner_count",
            "mpi_fd_count",
            "mpi_owner_count",
            "rkipc_process_count",
            "rkipc_mpi_owner_count",
        ):
            self.assertIs(type(audio[field]), int, field)
            self.assertGreaterEqual(audio[field], 0, field)

        gate = document["execution_gate"]
        self.assertIs(gate["safe_to_execute"], False)
        self.assertEqual(gate["exclusivity"], "unproven")
        self.assertEqual(gate["kernel_log_continuity"], "unproven")
        self.assertEqual(gate["reason_codes"][:3], BASE_REASON_CODES)
        kernel_log = document["kernel_log"]
        self.assertEqual(kernel_log["dmesg_evidence_scope"], "snapshot_only")
        self.assertEqual(kernel_log["kmsg_stream_semantics"], "unverified")
        self.assertIs(kernel_log["continuous_evidence_ready"], False)
        self.assertIs(document["service_control"]["automation_safe"], False)

        self.assertEqual(set(document["validation"]), VALIDATION_FIELDS)
        self.assertTrue(
            all(value is False for value in document["validation"].values())
        )

    def assert_no_forbidden_calls(self, fixture: FakePreflightEnvironment) -> None:
        invoked = {call["command"] for call in fixture.calls()}
        self.assertEqual(invoked & FORBIDDEN_COMMANDS, set(), fixture.calls())

    def test_help_and_invalid_arguments_do_not_probe(self) -> None:
        cases = (
            (("--help",), 0),
            (("-h",), 0),
            (("--unknown-SECRET_OPTION",), 2),
            (("--help", "extra-SECRET_ARGUMENT"), 2),
            (("-h", "--help"), 2),
        )
        for index, (arguments, expected_returncode) in enumerate(cases):
            with self.subTest(arguments=arguments), tempfile.TemporaryDirectory(
                prefix=f"boompi-mpi-preflight-arguments-{index}-"
            ) as temporary:
                fixture = FakePreflightEnvironment(Path(temporary))
                before = tree_snapshot(fixture.root)

                completed = fixture.run(*arguments)

                self.assertEqual(completed.returncode, expected_returncode)
                self.assertEqual(fixture.calls(), [])
                self.assertEqual(tree_snapshot(fixture.root), before)
                self.assertNotIn(str(fixture.root), completed.stdout + completed.stderr)
                self.assertNotIn("SECRET", completed.stdout + completed.stderr)
                if expected_returncode == 0:
                    self.assertIn("read-only", completed.stdout)
                else:
                    self.assertIn("invalid arguments", completed.stderr)

    def test_normal_snapshot_has_no_owners_and_remains_blocked(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="boompi-mpi-preflight-normal-"
        ) as temporary:
            fixture = FakePreflightEnvironment(Path(temporary))

            document = self.parse_report(fixture.run())

            self.assert_common_contract(document)
            self.assertEqual(document["probe_status"], "complete")
            self.assertTrue(document["audio"]["proc_fd_scan_complete"])
            for field in (
                "snd_fd_count",
                "snd_owner_count",
                "mpi_fd_count",
                "mpi_owner_count",
                "rkipc_process_count",
                "rkipc_mpi_owner_count",
            ):
                self.assertEqual(document["audio"][field], 0, field)
            self.assertFalse(document["execution_gate"]["snapshot_has_pcm_owner"])
            self.assertFalse(document["execution_gate"]["snapshot_has_mpi_owner"])
            self.assertEqual(
                document["execution_gate"]["reason_codes"], BASE_REASON_CODES
            )
            self.assert_no_forbidden_calls(fixture)

    def test_rkipc_holding_only_mpi_vsys_is_detected_and_blocked(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="boompi-mpi-preflight-rkipc-"
        ) as temporary:
            fixture = FakePreflightEnvironment(Path(temporary))
            fixture.add_process(900001, "rkipc", ("/dev/mpi/vsys",))

            document = self.parse_report(fixture.run())

            self.assert_common_contract(document)
            self.assertEqual(document["probe_status"], "complete")
            self.assertEqual(document["audio"]["snd_fd_count"], 0)
            self.assertEqual(document["audio"]["snd_owner_count"], 0)
            self.assertEqual(document["audio"]["mpi_fd_count"], 1)
            self.assertEqual(document["audio"]["mpi_owner_count"], 1)
            self.assertEqual(document["audio"]["rkipc_process_count"], 1)
            self.assertEqual(document["audio"]["rkipc_mpi_owner_count"], 1)
            self.assertFalse(document["execution_gate"]["snapshot_has_pcm_owner"])
            self.assertTrue(document["execution_gate"]["snapshot_has_mpi_owner"])
            self.assertEqual(
                document["execution_gate"]["reason_codes"],
                BASE_REASON_CODES
                + [
                    "rkipc_process_present",
                    "rkipc_mpi_device_owner_present",
                    "mpi_device_owner_present",
                ],
            )
            self.assert_no_forbidden_calls(fixture)

    def test_snd_owner_is_detected_and_blocked(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="boompi-mpi-preflight-snd-owner-"
        ) as temporary:
            fixture = FakePreflightEnvironment(Path(temporary))
            fixture.add_process(900002, "audiod", ("/dev/snd/pcmC0D0c",))

            document = self.parse_report(fixture.run())

            self.assert_common_contract(document)
            self.assertEqual(document["audio"]["snd_fd_count"], 1)
            self.assertEqual(document["audio"]["snd_owner_count"], 1)
            self.assertEqual(document["audio"]["mpi_fd_count"], 0)
            self.assertEqual(document["audio"]["mpi_owner_count"], 0)
            self.assertTrue(document["execution_gate"]["snapshot_has_pcm_owner"])
            self.assertFalse(document["execution_gate"]["snapshot_has_mpi_owner"])
            self.assertEqual(
                document["execution_gate"]["reason_codes"],
                BASE_REASON_CODES + ["snd_device_owner_present"],
            )
            self.assert_no_forbidden_calls(fixture)

    def test_proc_fd_scan_failure_is_incomplete_and_blocked(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="boompi-mpi-preflight-incomplete-"
        ) as temporary:
            fixture = FakePreflightEnvironment(Path(temporary))
            fixture.add_process(900003, "worker", ("/dev/snd/pcmC0D0c",))

            document = self.parse_report(
                fixture.run(
                    BOOMPI_PREFLIGHT_TEST_READLINK_FAILURE_SUFFIX=(
                        "/proc/900003/fd/3"
                    )
                )
            )

            self.assert_common_contract(document)
            self.assertEqual(document["probe_status"], "incomplete")
            self.assertFalse(document["audio"]["proc_fd_scan_complete"])
            self.assertFalse(document["execution_gate"]["snapshot_has_pcm_owner"])
            self.assertEqual(
                document["execution_gate"]["reason_codes"],
                BASE_REASON_CODES
                + ["preflight_collection_incomplete", "proc_fd_scan_incomplete"],
            )
            self.assert_no_forbidden_calls(fixture)

    def test_readable_dmesg_without_follow_option_is_not_continuous(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="boompi-mpi-preflight-dmesg-"
        ) as temporary:
            fixture = FakePreflightEnvironment(Path(temporary))

            document = self.parse_report(
                fixture.run(
                    BOOMPI_PREFLIGHT_TEST_DMESG_HELP=(
                        "Usage: dmesg [options]\n"
                        "  -c, --read-clear  read and clear messages\n"
                        "  -H, --human       human readable output\n"
                    )
                )
            )

            self.assert_common_contract(document)
            kernel_log = document["kernel_log"]
            self.assertTrue(kernel_log["dmesg_readable"])
            self.assertFalse(kernel_log["dmesg_follow_option_listed"])
            self.assertTrue(kernel_log["kmsg_present"])
            self.assertTrue(kernel_log["kmsg_readable"])
            self.assertFalse(kernel_log["continuous_evidence_ready"])
            self.assertEqual(
                document["execution_gate"]["reason_codes"],
                BASE_REASON_CODES + ["dmesg_follow_option_not_listed"],
            )
            dmesg_calls = fixture.calls("dmesg")
            self.assertIn({"command": "dmesg", "argv": []}, dmesg_calls)
            self.assertIn({"command": "dmesg", "argv": ["--help"]}, dmesg_calls)
            self.assertFalse(
                any("-w" in call["argv"] or "--follow" in call["argv"] for call in dmesg_calls)
            )
            self.assert_no_forbidden_calls(fixture)

    def test_rklunch_stop_lexical_killall_risk_is_never_executed(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="boompi-mpi-preflight-service-"
        ) as temporary:
            fixture = FakePreflightEnvironment(Path(temporary))
            write_text(
                fixture.root,
                "etc/init.d/S21appinit",
                "case \"$1\" in\n"
                "  start) /oem/usr/bin/RkLunch.sh ;;\n"
                "  stop) /oem/usr/bin/RkLunch-stop.sh ;;\n"
                "esac\n",
            )
            write_text(
                fixture.root,
                "oem/usr/bin/RkLunch.sh",
                "#!/bin/sh\nprintf '%s\\n' start\n",
            )
            write_text(
                fixture.root,
                "oem/usr/bin/RkLunch-stop.sh",
                "#!/bin/sh\nkillall rkipc\nkillall udhcpc\n",
            )
            before = tree_snapshot(fixture.root)

            document = self.parse_report(fixture.run())

            self.assert_common_contract(document)
            service = document["service_control"]
            self.assertTrue(service["s21appinit_present"])
            self.assertTrue(service["s21appinit_start_lexical"])
            self.assertTrue(service["s21appinit_stop_lexical"])
            self.assertTrue(service["rklunch_start_script_present"])
            self.assertTrue(service["rklunch_stop_script_present"])
            self.assertTrue(service["stop_killall_rkipc_lexical"])
            self.assertTrue(service["stop_udhcpc_lexical"])
            self.assertFalse(service["automation_safe"])
            self.assertEqual(
                document["execution_gate"]["reason_codes"],
                BASE_REASON_CODES + ["service_stop_lexical_risk_detected"],
            )
            self.assertEqual(tree_snapshot(fixture.root), before)
            self.assert_no_forbidden_calls(fixture)

    def test_report_is_redacted_and_fixture_is_never_modified(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="boompi-mpi-preflight-privacy-"
        ) as temporary:
            fixture = FakePreflightEnvironment(Path(temporary))
            fixture.add_process(900004, "worker")
            write_bytes(
                fixture.root,
                "proc/900004/cmdline",
                b"private-daemon\x00--token\x00SECRET_COMMAND_LINE\x00",
            )
            write_bytes(
                fixture.root,
                "proc/900004/environ",
                b"API_TOKEN=SECRET_PROCESS_ENVIRONMENT\x00",
            )
            before = tree_snapshot(fixture.root)

            completed = fixture.run(
                BOOMPI_PREFLIGHT_TEST_DMESG_TEXT=(
                    "[ 1.0] SECRET_DMESG de:ad:be:ef:00:01 192.168.7.2\n"
                ),
                SECRET_HOST_ENVIRONMENT="SECRET_PARENT_ENVIRONMENT",
            )
            document = self.parse_report(completed)

            self.assert_common_contract(document)
            self.assertEqual(tree_snapshot(fixture.root), before)
            self.assert_no_forbidden_calls(fixture)
            forbidden_output = (
                str(fixture.base),
                "private-user-home",
                "900004",
                "SECRET_COMMAND_LINE",
                "SECRET_PROCESS_ENVIRONMENT",
                "SECRET_DMESG",
                "SECRET_PARENT_ENVIRONMENT",
                "de:ad:be:ef:00:01",
                "192.168.7.2",
                "/dev/snd/",
                "/dev/mpi/",
            )
            for secret in forbidden_output:
                with self.subTest(secret=secret):
                    self.assertNotIn(secret, completed.stdout)

    def test_target_script_has_valid_posix_shell_syntax(self) -> None:
        completed = subprocess.run(
            ["/bin/sh", "-n", str(PREFLIGHT)],
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)


if __name__ == "__main__":
    unittest.main()
