#!/usr/bin/env python3
"""Offline contract tests for the opt-in direct-ALSA full-duplex HIL probe."""

import json
import os
from pathlib import Path
import re
import shlex
import stat
import subprocess
import tempfile
import time
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
HIL_SCRIPT = (
    REPOSITORY_ROOT / "scripts" / "hil" / "rv1106_alsa_full_duplex.sh"
)

CAPTURE_BYTES = 48_000 * 2 * 2 * 6
PLAYBACK_BYTES = 48_000 * 2 * 2 * 4
EXECUTION_OPT_INS = ("--execute", "--allow-pcm-io", "--allow-mixer-write")


def write_text(root: Path, relative: str, text: str = "") -> Path:
    target = root / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding="utf-8")
    return target


def write_command(bin_dir: Path, name: str, body: str) -> None:
    target = write_text(bin_dir, name, "#!/bin/sh\n" + body)
    target.chmod(target.stat().st_mode | stat.S_IXUSR)


def tree_snapshot(root: Path) -> dict[str, tuple]:
    snapshot = {}
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


def process_exists(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


class FakeHilEnvironment:
    """A fake command environment that never opens a real ALSA device."""

    def __init__(self, base: Path):
        self.base = base
        self.bin_dir = base / "fake-bin"
        self.state_dir = base / "fake-state"
        self.call_log = self.state_dir / "calls.log"
        self.mixer_state = self.state_dir / "mixer-state"
        self.uptime_file = self.state_dir / "uptime"
        self.uptime_ms = self.state_dir / "uptime-ms"
        self.bin_dir.mkdir(parents=True)
        self.state_dir.mkdir(parents=True)
        self.call_log.write_text("", encoding="utf-8")
        self.mixer_state.write_text("2\n", encoding="utf-8")
        self.uptime_file.write_text("1000.00 0.00\n", encoding="utf-8")
        self.uptime_ms.write_text("1000000\n", encoding="utf-8")
        script_text = HIL_SCRIPT.read_text(encoding="utf-8")
        uptime_read = "</proc/uptime"
        if script_text.count(uptime_read) != 1:
            raise AssertionError("expected exactly one production uptime read")
        script_text = script_text.replace(
            uptime_read, '<"$BOOMPI_HIL_TEST_UPTIME"'
        )
        self.script_under_test = write_text(
            self.base, "rv1106_alsa_full_duplex-under-test.sh", script_text
        )
        self.script_under_test.chmod(
            self.script_under_test.stat().st_mode | stat.S_IXUSR
        )
        self._install_commands()

    def _install_commands(self) -> None:
        log_call = (
            "printf '%s|%s\\n' \"$1\" \"$2\" "
            '>> \"$BOOMPI_HIL_TEST_CALL_LOG\"\n'
        )

        write_command(
            self.bin_dir,
            "amixer",
            "set -u\n"
            + log_call.replace('"$1"', "amixer").replace('"$2"', '"$*"')
            + 'state="$BOOMPI_HIL_TEST_STATE_DIR/mixer-state"\n'
            + 'current="$(sed -n \'1p\' "$state")"\n'
            + "print_control() {\n"
            + "  echo \"numid=42,iface=MIXER,name='DAC Control Manually'\"\n"
            + "  echo \"; type=ENUMERATED,access=rw---R--,values=1,items=3\"\n"
            + "  echo \"; Item #0 'None'\"\n"
            + "  echo \"; Item #1 'Off'\"\n"
            + "  echo \"; Item #2 'On'\"\n"
            + "  echo \"  : values=$current\"\n"
            + "}\n"
            + 'case " $* " in\n'
            + "  *\\ controls\\ *|*\\ contents\\ *)\n"
            + "    echo \"numid=42,iface=MIXER,name='DAC Control Manually'\"\n"
            + "    exit 0\n"
            + "    ;;\n"
            + "  *\\ cget\\ *|*\\ sget\\ *)\n"
            + "    print_control\n"
            + "    exit 0\n"
            + "    ;;\n"
            + "  *\\ cset\\ *|*\\ sset\\ *)\n"
            + "    value=\n"
            + "    for argument do value=$argument; done\n"
            + '    case "$value" in\n'
            + "      0|None) new_value=0 ;;\n"
            + "      1|Off) new_value=1 ;;\n"
            + "      2|On) new_value=2 ;;\n"
            + "      *) exit 24 ;;\n"
            + "    esac\n"
            + '    if [ "${AMIXER_FAIL_RESTORE:-0}" = 1 ] '
            + '&& [ "$current" = 1 ] && [ "$new_value" = 2 ]; then\n'
            + "      exit 29\n"
            + "    fi\n"
            + '    printf \'%s\\n\' "$new_value" > "$state"\n'
            + '    if [ "${AMIXER_ARTIFACT_ACTION:-}" = move ] '
            + '&& [ "$current" = 1 ] && [ "$new_value" = 2 ]; then\n'
            + '      artifact_path=${AMIXER_ARTIFACT_DIR:-}\n'
            + '      [ -n "$artifact_path" ] || exit 30\n'
            + '      mv "$artifact_path" "$artifact_path.moved" || exit 30\n'
            + "    fi\n"
            + "    current=$new_value\n"
            + "    print_control\n"
            + "    exit 0\n"
            + "    ;;\n"
            + "esac\n"
            + "exit 0\n",
        )

        write_command(
            self.bin_dir,
            "arecord",
            "set -u\n"
            + log_call.replace('"$1"', "arecord").replace('"$2"', '"$*"')
            + 'case " $* " in\n'
            + "  *\\ --dump-hw-params\\ *)\n"
            + "    echo 'ACCESS:  RW_INTERLEAVED'\n"
            + "    echo 'FORMAT:  S16_LE'\n"
            + "    echo 'CHANNELS: [2 2]'\n"
            + "    echo 'RATE: [48000 48000]'\n"
            + "    exit 0\n"
            + "    ;;\n"
            + "esac\n"
            + 'printf \'%s\\n\' "$$" > '
            + '"$BOOMPI_HIL_TEST_STATE_DIR/arecord-pid"\n'
            + "output=\n"
            + "consume_next=0\n"
            + "for argument do\n"
            + '  if [ "$consume_next" = 1 ]; then\n'
            + "    consume_next=0\n"
            + "    continue\n"
            + "  fi\n"
            + '  case "$argument" in\n'
            + "    -D|-t|-f|-r|-c|-d|--device|--file-type|--format|--rate|"
            + "--channels|--duration|--period-size|--buffer-size) consume_next=1 ;;\n"
            + "    -*) ;;\n"
            + '    *) output="$argument" ;;\n'
            + "  esac\n"
            + "done\n"
            + 'if [ "${ARECORD_HANG:-0}" = 1 ]; then\n'
            + "  trap 'printf \"%s|%s\\n\" arecord \"$$\" >> "
            + '"$BOOMPI_HIL_TEST_STATE_DIR/terminated-pids.log"; '
            + "exit 143' HUP INT TERM\n"
            + "  while :; do :; done\n"
            + "fi\n"
            + 'clock="$BOOMPI_HIL_TEST_STATE_DIR/uptime-ms"\n'
            + 'virtual_start="$(sed -n \'1p\' "$clock")"\n'
            + 'case "$virtual_start" in \'\'|*[!0-9]*) exit 31 ;; esac\n'
            + "virtual_end=$((virtual_start + 6000))\n"
            + "while :; do\n"
            + '  virtual_now="$(sed -n \'1p\' "$clock")"\n'
            + '  case "$virtual_now" in \'\'|*[!0-9]*) /bin/sleep 0.001; continue ;; esac\n'
            + '  [ "$virtual_now" -ge "$virtual_end" ] && break\n'
            + "  /bin/sleep 0.001\n"
            + "done\n"
            + 'bytes="${CAPTURE_BYTES_OVERRIDE:-1152000}"\n'
            + 'if [ -n "$output" ]; then\n'
            + '  /bin/dd if=/dev/zero of="$output" bs="$bytes" count=1 '
            + "status=none\n"
            + "else\n"
            + '  /bin/dd if=/dev/zero bs="$bytes" count=1 status=none\n'
            + "fi\n"
            + 'exit "${ARECORD_RC:-0}"\n',
        )

        write_command(
            self.bin_dir,
            "aplay",
            "set -u\n"
            + log_call.replace('"$1"', "aplay").replace('"$2"', '"$*"')
            + 'case " $* " in\n'
            + "  *\\ --dump-hw-params\\ *) exit 0 ;;\n"
            + "esac\n"
            + 'printf \'%s\\n\' "$$" > '
            + '"$BOOMPI_HIL_TEST_STATE_DIR/aplay-pid"\n'
            + "input=\n"
            + "for argument do input=$argument; done\n"
            + 'if [ -f "$input" ]; then\n'
            + '  wc -c < "$input" > '
            + '"$BOOMPI_HIL_TEST_STATE_DIR/aplay-input-bytes"\n'
            + "fi\n"
            + 'if [ "${APLAY_HANG:-0}" = 1 ]; then\n'
            + "  trap 'printf \"%s|%s\\n\" aplay \"$$\" >> "
            + '"$BOOMPI_HIL_TEST_STATE_DIR/terminated-pids.log"; '
            + "exit 143' HUP INT TERM\n"
            + "  while :; do :; done\n"
            + "fi\n"
            + 'clock="$BOOMPI_HIL_TEST_STATE_DIR/uptime-ms"\n'
            + 'virtual_start="$(sed -n \'1p\' "$clock")"\n'
            + 'case "$virtual_start" in \'\'|*[!0-9]*) exit 31 ;; esac\n'
            + "virtual_end=$((virtual_start + 4000))\n"
            + "while :; do\n"
            + '  virtual_now="$(sed -n \'1p\' "$clock")"\n'
            + '  case "$virtual_now" in \'\'|*[!0-9]*) /bin/sleep 0.001; continue ;; esac\n'
            + '  [ "$virtual_now" -ge "$virtual_end" ] && break\n'
            + "  /bin/sleep 0.001\n"
            + "done\n"
            + 'exit "${APLAY_RC:-0}"\n',
        )

        write_command(
            self.bin_dir,
            "fuser",
            "set -u\n"
            + log_call.replace('"$1"', "fuser").replace('"$2"', '"$*"')
            + 'if [ -n "${FUSER_OWNER_PID:-}" ]; then\n'
            + '  printf \'%s\\n\' "$FUSER_OWNER_PID"\n'
            + "  exit 0\n"
            + "fi\n"
            + "exit 1\n",
        )

        write_command(
            self.bin_dir,
            "dmesg",
            "set -u\n"
            + log_call.replace('"$1"', "dmesg").replace('"$2"', '"$*"')
            + 'counter="$BOOMPI_HIL_TEST_STATE_DIR/dmesg-count"\n'
            + 'count="$(sed -n \'1p\' "$counter" 2>/dev/null || true)"\n'
            + 'case "$count" in \'\'|*[!0-9]*) count=0 ;; esac\n'
            + "count=$((count + 1))\n"
            + 'printf \'%s\\n\' "$count" > "$counter"\n'
            + 'if [ "${DMESG_DENIED:-0}" = 1 ]; then exit 1; fi\n'
            + 'if [ "${DMESG_RING_WRAP:-0}" = 1 ]; then\n'
            + '  if [ "$count" -eq 1 ]; then\n'
            + "    echo '[    1.000000] first snapshot line one'\n"
            + "    echo '[    2.000000] first snapshot line two'\n"
            + "  else\n"
            + "    echo '[   99.000000] ring buffer replaced the prefix'\n"
            + "  fi\n"
            + "  exit 0\n"
            + "fi\n"
            + "echo '[    1.000000] historical underrun retained in both snapshots'\n"
            + 'if [ "$count" -gt 1 ] && [ "${DMESG_NEW_XRUN:-0}" = 1 ]; then\n'
            + "  echo '[   42.000000] new playback xrun in HIL window'\n"
            + "fi\n",
        )

        write_command(
            self.bin_dir,
            "sleep",
            "set -u\n"
            + log_call.replace('"$1"', "sleep").replace('"$2"', '"$*"')
            + "/bin/sleep 0.03\n",
        )

        write_command(
            self.bin_dir,
            "usleep",
            "set -u\n"
            + log_call.replace('"$1"', "usleep").replace('"$2"', '"$*"')
            + '[ "$#" -eq 1 ] || exit 64\n'
            + 'delay_us=$1\n'
            + 'case "$delay_us" in 50000|100000) ;; *) exit 64 ;; esac\n'
            + "delay_ms=$((delay_us / 1000))\n"
            + 'clock="$BOOMPI_HIL_TEST_STATE_DIR/uptime-ms"\n'
            + 'value="$(sed -n \'1p\' "$clock")"\n'
            + 'case "$value" in \'\'|*[!0-9]*) exit 64 ;; esac\n'
            + "value=$((value + delay_ms))\n"
            + 'clock_temp="$clock.$$"\n'
            + 'printf \'%s\\n\' "$value" > "$clock_temp"\n'
            + 'mv "$clock_temp" "$clock"\n'
            + "seconds=$((value / 1000))\n"
            + "centiseconds=$(((value % 1000) / 10))\n"
            + 'printf \'%s.%02d 0.00\\n\' "$seconds" "$centiseconds" '
            + '> "$BOOMPI_HIL_TEST_UPTIME"\n'
            + "/bin/sleep 0.001\n",
        )

        write_command(
            self.bin_dir,
            "readlink",
            "set -u\n"
            + "last=\n"
            + "for argument do last=$argument; done\n"
            + 'case "$last" in\n'
            + '  /dev/snd/pcmC*|/dev/snd/controlC*) printf \'%s\\n\' "$last" ;;\n'
            + "  /proc/[0-9]*/fd/*)\n"
            + '    pid=${last#/proc/}\n'
            + '    pid=${pid%%/*}\n'
            + '    capture_pid="$(sed -n \'1p\' '
            + '"$BOOMPI_HIL_TEST_STATE_DIR/arecord-pid" 2>/dev/null || true)"\n'
            + '    playback_pid="$(sed -n \'1p\' '
            + '"$BOOMPI_HIL_TEST_STATE_DIR/aplay-pid" 2>/dev/null || true)"\n'
            + '    if [ -n "$capture_pid" ] && [ "$pid" = "$capture_pid" ]; then\n'
            + "      echo /dev/snd/pcmC7D3c\n"
            + '    elif [ -n "$playback_pid" ] && [ "$pid" = "$playback_pid" ]; then\n'
            + "      echo /dev/snd/pcmC7D4p\n"
            + "    else\n"
            + '      exec /usr/bin/readlink "$@"\n'
            + "    fi\n"
            + "    ;;\n"
            + "  *) exec /usr/bin/readlink \"$@\" ;;\n"
            + "esac\n",
        )

    def environment(self, **overrides: str) -> dict[str, str]:
        environment = os.environ.copy()
        environment.update(
            {
                "BOOMPI_HIL_TEST_CALL_LOG": str(self.call_log),
                "BOOMPI_HIL_TEST_STATE_DIR": str(self.state_dir),
                "BOOMPI_HIL_TEST_UPTIME": str(self.uptime_file),
                "LC_ALL": "C",
                "PATH": os.pathsep.join((str(self.bin_dir), environment["PATH"])),
            }
        )
        environment.update(overrides)
        return environment

    def run(self, *arguments: str, **environment_overrides: str):
        return subprocess.run(
            ["/bin/sh", str(self.script_under_test), *arguments],
            check=False,
            capture_output=True,
            text=True,
            timeout=15,
            env=self.environment(**environment_overrides),
            cwd=self.base,
        )

    def base_arguments(self, artifact_dir: Path) -> list[str]:
        return [
            "--capture-pcm",
            "hw:7,3",
            "--playback-pcm",
            "hw:7,4",
            "--mixer-card",
            "7",
            "--dac-control",
            "DAC Control Manually",
            "--artifact-dir",
            str(artifact_dir),
        ]

    def calls(self, command: str | None = None) -> list[tuple[str, str]]:
        result = []
        for line in self.call_log.read_text(encoding="utf-8").splitlines():
            name, separator, arguments = line.partition("|")
            if separator and (command is None or name == command):
                result.append((name, arguments))
        return result

    def mixer_value(self) -> str:
        return self.mixer_state.read_text(encoding="utf-8").strip()


class Rv1106AlsaFullDuplexHilTest(unittest.TestCase):
    maxDiff = None

    def assert_no_pcm_or_mixer_mutation(self, fixture: FakeHilEnvironment) -> None:
        self.assertFalse(fixture.calls("arecord"))
        self.assertFalse(fixture.calls("aplay"))
        mixer_writes = [
            arguments
            for _, arguments in fixture.calls("amixer")
            if re.search(r"(?:^|\s)[cs]set(?:\s|$)", arguments)
        ]
        self.assertEqual(mixer_writes, [])
        self.assertEqual(fixture.mixer_value(), "2")

    def assert_option(
        self, tokens: list[str], names: tuple[str, ...], expected: str
    ) -> None:
        for index, token in enumerate(tokens):
            if token in names and index + 1 < len(tokens):
                if tokens[index + 1] == expected:
                    return
            for name in names:
                if token == f"{name}={expected}":
                    return
        self.fail(f"missing option {names!r}={expected!r} in {tokens!r}")

    def assert_mixer_was_forced_off_and_restored(
        self, fixture: FakeHilEnvironment
    ) -> None:
        writes = [
            arguments
            for _, arguments in fixture.calls("amixer")
            if re.search(r"(?:^|\s)[cs]set(?:\s|$)", arguments)
        ]
        off_index = next(
            (
                index
                for index, arguments in enumerate(writes)
                if re.search(r"(?:^|\s)(?:1|Off)$", arguments)
            ),
            None,
        )
        restore_index = next(
            (
                index
                for index, arguments in enumerate(writes)
                if re.search(r"(?:^|\s)(?:2|On)$", arguments)
            ),
            None,
        )
        self.assertIsNotNone(off_index, writes)
        self.assertIsNotNone(restore_index, writes)
        self.assertLess(off_index, restore_index, writes)

    def test_default_is_a_zero_write_dry_run(self):
        with tempfile.TemporaryDirectory(prefix="boompi-hil-dry-run-") as temporary:
            fixture = FakeHilEnvironment(Path(temporary))
            artifact_dir = fixture.base / "new-artifacts"
            before = tree_snapshot(fixture.base)

            completed = fixture.run(*fixture.base_arguments(artifact_dir))

            self.assertEqual(completed.returncode, 0, completed.stderr)
            dry_run = json.loads(completed.stdout)
            self.assertEqual(dry_run["mode"], "dry_run")
            self.assertFalse(dry_run["mutated"])
            self.assertFalse(os.path.lexists(artifact_dir))
            self.assertEqual(fixture.calls("amixer"), [])
            self.assertEqual(fixture.calls("arecord"), [])
            self.assertEqual(fixture.calls("aplay"), [])
            self.assertEqual(fixture.mixer_value(), "2")
            self.assertEqual(tree_snapshot(fixture.base), before)

    def test_execute_requires_all_three_opt_ins_before_any_mutation(self):
        incomplete_opt_ins = (
            ("--execute",),
            ("--execute", "--allow-pcm-io"),
            ("--execute", "--allow-mixer-write"),
        )
        for index, opt_ins in enumerate(incomplete_opt_ins):
            with self.subTest(opt_ins=opt_ins), tempfile.TemporaryDirectory(
                prefix=f"boompi-hil-opt-in-{index}-"
            ) as temporary:
                fixture = FakeHilEnvironment(Path(temporary))
                artifact_dir = fixture.base / "new-artifacts"

                completed = fixture.run(
                    *fixture.base_arguments(artifact_dir), *opt_ins
                )

                self.assertEqual(completed.returncode, 2, completed.stderr)
                self.assertFalse(os.path.lexists(artifact_dir))
                self.assert_no_pcm_or_mixer_mutation(fixture)

    def test_mixer_card_requires_canonical_uint_in_dry_run_and_execute(self):
        modes = (("dry-run", ()), ("execute", EXECUTION_OPT_INS))
        for mode, opt_ins in modes:
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(
                prefix=f"boompi-hil-mixer-card-{mode}-"
            ) as temporary:
                fixture = FakeHilEnvironment(Path(temporary))
                artifact_dir = fixture.base / "new-artifacts"
                arguments = fixture.base_arguments(artifact_dir)
                arguments[arguments.index("--mixer-card") + 1] = "07"
                before = tree_snapshot(fixture.base)

                completed = fixture.run(*arguments, *opt_ins)

                self.assertEqual(completed.returncode, 2, completed.stderr)
                self.assertFalse(os.path.lexists(artifact_dir))
                self.assertEqual(fixture.calls(), [])
                self.assert_no_pcm_or_mixer_mutation(fixture)
                self.assertEqual(tree_snapshot(fixture.base), before)

    def test_pcm_names_reject_newlines_and_noncanonical_uints_without_writes(
        self,
    ):
        invalid_values = (
            ("capture-newline", "--capture-pcm", "hw:7,3\ntrailing"),
            ("playback-newline", "--playback-pcm", "hw:7,4\ntrailing"),
            ("capture-leading-zero", "--capture-pcm", "hw:07,3"),
            ("playback-leading-zero", "--playback-pcm", "hw:7,04"),
        )
        modes = (("dry-run", ()), ("execute", EXECUTION_OPT_INS))
        for case, option, invalid_value in invalid_values:
            for mode, opt_ins in modes:
                with self.subTest(case=case, mode=mode), tempfile.TemporaryDirectory(
                    prefix=f"boompi-hil-pcm-{case}-{mode}-"
                ) as temporary:
                    fixture = FakeHilEnvironment(Path(temporary))
                    artifact_dir = fixture.base / "new-artifacts"
                    arguments = fixture.base_arguments(artifact_dir)
                    arguments[arguments.index(option) + 1] = invalid_value
                    before = tree_snapshot(fixture.base)

                    completed = fixture.run(*arguments, *opt_ins)

                    self.assertEqual(completed.returncode, 2, completed.stderr)
                    self.assertFalse(os.path.lexists(artifact_dir))
                    self.assertEqual(fixture.calls(), [])
                    self.assert_no_pcm_or_mixer_mutation(fixture)
                    self.assertEqual(tree_snapshot(fixture.base), before)

    def test_artifact_parent_symlink_is_rejected_without_traversal_or_writes(
        self,
    ):
        with tempfile.TemporaryDirectory(
            prefix="boompi-hil-symlink-parent-"
        ) as temporary:
            fixture = FakeHilEnvironment(Path(temporary))
            real_parent = fixture.base / "real-parent"
            linked_parent = fixture.base / "linked-parent"
            real_parent.mkdir()
            linked_parent.symlink_to(real_parent, target_is_directory=True)
            artifact_dir = linked_parent / "new-artifacts"
            real_artifact_dir = real_parent / "new-artifacts"
            before = tree_snapshot(fixture.base)

            completed = fixture.run(
                *fixture.base_arguments(artifact_dir), *EXECUTION_OPT_INS
            )

            self.assertEqual(completed.returncode, 2, completed.stderr)
            self.assertFalse(os.path.lexists(artifact_dir))
            self.assertFalse(os.path.lexists(real_artifact_dir))
            self.assertEqual(fixture.calls(), [])
            self.assert_no_pcm_or_mixer_mutation(fixture)
            self.assertEqual(tree_snapshot(fixture.base), before)

    def test_artifact_dir_must_be_absolute_new_and_not_a_symlink(self):
        cases = ("relative", "directory", "file", "dangling-symlink")
        for case in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory(
                prefix=f"boompi-hil-artifact-{case}-"
            ) as temporary:
                fixture = FakeHilEnvironment(Path(temporary))
                if case == "relative":
                    artifact_dir = Path("relative-hil-artifacts")
                elif case == "directory":
                    artifact_dir = fixture.base / "already-exists"
                    artifact_dir.mkdir()
                elif case == "file":
                    artifact_dir = fixture.base / "already-exists"
                    artifact_dir.write_text("owned", encoding="utf-8")
                else:
                    artifact_dir = fixture.base / "artifact-link"
                    artifact_dir.symlink_to(fixture.base / "missing-target")

                completed = fixture.run(
                    *fixture.base_arguments(artifact_dir), *EXECUTION_OPT_INS
                )

                self.assertEqual(completed.returncode, 2, completed.stderr)
                self.assert_no_pcm_or_mixer_mutation(fixture)

    def test_occupied_pcm_exits_three_without_opening_pcm_or_writing_mixer(self):
        with tempfile.TemporaryDirectory(prefix="boompi-hil-occupied-") as temporary:
            fixture = FakeHilEnvironment(Path(temporary))
            artifact_dir = fixture.base / "new-artifacts"

            completed = fixture.run(
                *fixture.base_arguments(artifact_dir),
                *EXECUTION_OPT_INS,
                FUSER_OWNER_PID="4321",
            )

            self.assertEqual(
                completed.returncode,
                3,
                f"{completed.stderr}\nrecorded calls: {fixture.calls()!r}",
            )
            self.assert_no_pcm_or_mixer_mutation(fixture)
            fuser_arguments = "\n".join(
                arguments for _, arguments in fixture.calls("fuser")
            )
            self.assertIn("/dev/snd/pcmC7D3c", fuser_arguments)
            self.assertIn("/dev/snd/pcmC7D4p", fuser_arguments)

    def test_success_uses_fixed_format_durations_and_restores_mixer(self):
        with tempfile.TemporaryDirectory(prefix="boompi-hil-success-") as temporary:
            fixture = FakeHilEnvironment(Path(temporary))
            artifact_dir = fixture.base / "new-artifacts"

            completed = fixture.run(
                *fixture.base_arguments(artifact_dir), *EXECUTION_OPT_INS
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(fixture.mixer_value(), "2")
            self.assert_mixer_was_forced_off_and_restored(fixture)

            capture_calls = [
                shlex.split(arguments)
                for _, arguments in fixture.calls("arecord")
                if "hw:7,3" in arguments and "--dump-hw-params" not in arguments
            ]
            playback_calls = [
                shlex.split(arguments)
                for _, arguments in fixture.calls("aplay")
                if "hw:7,4" in arguments and "--dump-hw-params" not in arguments
            ]
            self.assertEqual(len(capture_calls), 1, capture_calls)
            self.assertEqual(len(playback_calls), 1, playback_calls)
            capture = capture_calls[0]
            playback = playback_calls[0]

            for tokens, device in ((capture, "hw:7,3"), (playback, "hw:7,4")):
                self.assert_option(tokens, ("-D", "--device"), device)
                self.assert_option(tokens, ("-t", "--file-type"), "raw")
                self.assert_option(tokens, ("-f", "--format"), "S16_LE")
                self.assert_option(tokens, ("-r", "--rate"), "48000")
                self.assert_option(tokens, ("-c", "--channels"), "2")
                self.assert_option(tokens, ("--period-size",), "480")
                self.assert_option(tokens, ("--buffer-size",), "1920")
            self.assert_option(capture, ("-d", "--duration"), "6")

            capture_file = artifact_dir / "capture.raw"
            self.assertEqual(capture_file.stat().st_size, CAPTURE_BYTES)
            playback_byte_file = fixture.state_dir / "aplay-input-bytes"
            self.assertEqual(
                int(playback_byte_file.read_text(encoding="utf-8")), PLAYBACK_BYTES
            )
            result_file = artifact_dir / "result.json"
            self.assertTrue(result_file.is_file())
            result = json.loads(result_file.read_text(encoding="utf-8"))
            self.assertEqual(result["configuration"]["rate_hz"], 48_000)
            self.assertEqual(result["configuration"]["format"], "S16_LE")
            self.assertEqual(result["configuration"]["channels"], 2)
            self.assertEqual(
                result["configuration"]["requested_period_frames"], 480
            )
            self.assertEqual(
                result["configuration"]["requested_buffer_frames"], 1_920
            )
            self.assertEqual(result["configuration"]["capture_seconds"], 6)
            self.assertEqual(result["configuration"]["playback_seconds"], 4)
            self.assertGreater(result["capture"]["pid"], 0)
            self.assertGreater(result["playback"]["pid"], 0)
            self.assertEqual(result["capture"]["exit_code"], 0)
            self.assertEqual(result["playback"]["exit_code"], 0)
            self.assertLessEqual(
                result["capture"]["start_monotonic_ms"],
                result["capture"]["open_monotonic_ms"],
            )
            self.assertLess(
                result["capture"]["open_monotonic_ms"],
                result["capture"]["end_monotonic_ms"],
            )
            self.assertLessEqual(
                result["playback"]["start_monotonic_ms"],
                result["playback"]["open_monotonic_ms"],
            )
            self.assertLess(
                result["playback"]["open_monotonic_ms"],
                result["playback"]["end_monotonic_ms"],
            )
            self.assertGreaterEqual(result["overlap_ms"], 3_000)
            self.assertTrue(result["mixer"]["restored"])
            self.assertEqual(result["overall"], "pass")
            self.assertEqual(artifact_dir.stat().st_mode & 0o077, 0)
            self.assertEqual(capture_file.stat().st_mode & 0o077, 0)

            self.assertGreaterEqual(len(fixture.calls("dmesg")), 2)

    def test_playback_failure_still_restores_mixer(self):
        with tempfile.TemporaryDirectory(prefix="boompi-hil-play-fail-") as temporary:
            fixture = FakeHilEnvironment(Path(temporary))
            artifact_dir = fixture.base / "new-artifacts"

            completed = fixture.run(
                *fixture.base_arguments(artifact_dir),
                *EXECUTION_OPT_INS,
                APLAY_RC="17",
            )

            self.assertEqual(completed.returncode, 5, completed.stderr)
            self.assertEqual(fixture.mixer_value(), "2")
            self.assert_mixer_was_forced_off_and_restored(fixture)

    def test_hung_pcm_hits_deadline_restores_mixer_and_only_kills_its_child(
        self,
    ):
        cases = (
            ("capture", "ARECORD_HANG", "arecord"),
            ("playback", "APLAY_HANG", "aplay"),
        )
        for case, hang_variable, hung_command in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory(
                prefix=f"boompi-hil-{case}-hang-"
            ) as temporary:
                fixture = FakeHilEnvironment(Path(temporary))
                artifact_dir = fixture.base / "new-artifacts"
                sentinel = subprocess.Popen(["/bin/sleep", "30"])
                try:
                    started = time.monotonic()
                    completed = fixture.run(
                        *fixture.base_arguments(artifact_dir),
                        *EXECUTION_OPT_INS,
                        **{hang_variable: "1"},
                    )
                    elapsed_seconds = time.monotonic() - started

                    self.assertEqual(completed.returncode, 5, completed.stderr)
                    self.assertLess(elapsed_seconds, 10)
                    self.assertIn("deadline", completed.stderr.lower())
                    self.assertIsNone(sentinel.poll(), "unrelated process was killed")
                    self.assertEqual(fixture.mixer_value(), "2")
                    self.assert_mixer_was_forced_off_and_restored(fixture)

                    child_pids = {
                        command: int(
                            (fixture.state_dir / f"{command}-pid")
                            .read_text(encoding="utf-8")
                            .strip()
                        )
                        for command in ("arecord", "aplay")
                    }
                    terminated_file = fixture.state_dir / "terminated-pids.log"
                    terminated = terminated_file.read_text(
                        encoding="utf-8"
                    ).splitlines()
                    self.assertEqual(
                        terminated,
                        [f"{hung_command}|{child_pids[hung_command]}"],
                    )
                    self.assertNotIn(str(sentinel.pid), "\n".join(terminated))
                    for child_pid in child_pids.values():
                        self.assertFalse(
                            process_exists(child_pid),
                            f"child PID {child_pid} survived bounded cleanup",
                        )
                finally:
                    if sentinel.poll() is None:
                        sentinel.terminate()
                    sentinel.wait(timeout=5)

    def test_artifact_loss_during_restore_cannot_turn_result_failure_into_pass(
        self,
    ):
        with tempfile.TemporaryDirectory(prefix="boompi-hil-artifact-loss-") as temporary:
            fixture = FakeHilEnvironment(Path(temporary))
            artifact_dir = fixture.base / "new-artifacts"
            moved_artifact_dir = Path(f"{artifact_dir}.moved")

            completed = fixture.run(
                *fixture.base_arguments(artifact_dir),
                *EXECUTION_OPT_INS,
                AMIXER_ARTIFACT_ACTION="move",
                AMIXER_ARTIFACT_DIR=str(artifact_dir),
            )

            self.assertEqual(completed.returncode, 5, completed.stderr)
            self.assertEqual(fixture.mixer_value(), "2")
            self.assert_mixer_was_forced_off_and_restored(fixture)
            self.assertFalse(artifact_dir.exists())
            self.assertTrue(moved_artifact_dir.is_dir())
            self.assertFalse((moved_artifact_dir / "result.json").exists())
            combined_output = (completed.stdout + completed.stderr).lower()
            self.assertNotIn("rv1106_alsa_full_duplex: pass", combined_output)
            self.assertIn("result artifact", completed.stderr.lower())

    def test_mixer_restore_failure_has_dedicated_exit_code(self):
        with tempfile.TemporaryDirectory(prefix="boompi-hil-restore-fail-") as temporary:
            fixture = FakeHilEnvironment(Path(temporary))
            artifact_dir = fixture.base / "new-artifacts"

            completed = fixture.run(
                *fixture.base_arguments(artifact_dir),
                *EXECUTION_OPT_INS,
                AMIXER_FAIL_RESTORE="1",
            )

            self.assertEqual(completed.returncode, 6, completed.stderr)
            self.assertEqual(fixture.mixer_value(), "1")
            writes = [
                arguments
                for _, arguments in fixture.calls("amixer")
                if re.search(r"(?:^|\s)[cs]set(?:\s|$)", arguments)
            ]
            self.assertTrue(
                any(re.search(r"(?:^|\s)(?:2|On)$", call) for call in writes),
                writes,
            )

    def test_wrong_capture_byte_count_fails_and_restores_mixer(self):
        with tempfile.TemporaryDirectory(prefix="boompi-hil-bytes-fail-") as temporary:
            fixture = FakeHilEnvironment(Path(temporary))
            artifact_dir = fixture.base / "new-artifacts"

            completed = fixture.run(
                *fixture.base_arguments(artifact_dir),
                *EXECUTION_OPT_INS,
                CAPTURE_BYTES_OVERRIDE="17",
            )

            self.assertEqual(completed.returncode, 5, completed.stderr)
            self.assertEqual(fixture.mixer_value(), "2")
            self.assert_mixer_was_forced_off_and_restored(fixture)

    def test_only_a_new_dmesg_xrun_fails_and_mixer_is_restored(self):
        with tempfile.TemporaryDirectory(prefix="boompi-hil-xrun-fail-") as temporary:
            fixture = FakeHilEnvironment(Path(temporary))
            artifact_dir = fixture.base / "new-artifacts"

            completed = fixture.run(
                *fixture.base_arguments(artifact_dir),
                *EXECUTION_OPT_INS,
                DMESG_NEW_XRUN="1",
            )

            self.assertEqual(completed.returncode, 5, completed.stderr)
            self.assertEqual(fixture.mixer_value(), "2")
            self.assert_mixer_was_forced_off_and_restored(fixture)

    def test_unavailable_or_wrapped_dmesg_is_inconclusive_and_restores_mixer(
        self,
    ):
        cases = (
            ("denied", {"DMESG_DENIED": "1"}),
            ("ring-wrap", {"DMESG_RING_WRAP": "1"}),
        )
        for case, environment in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory(
                prefix=f"boompi-hil-dmesg-{case}-"
            ) as temporary:
                fixture = FakeHilEnvironment(Path(temporary))
                artifact_dir = fixture.base / "new-artifacts"

                completed = fixture.run(
                    *fixture.base_arguments(artifact_dir),
                    *EXECUTION_OPT_INS,
                    **environment,
                )

                self.assertEqual(completed.returncode, 5, completed.stderr)
                self.assertEqual(fixture.mixer_value(), "2")
                self.assert_mixer_was_forced_off_and_restored(fixture)
                result = json.loads(
                    (artifact_dir / "result.json").read_text(encoding="utf-8")
                )
                self.assertEqual(result["dmesg"], "indeterminate")
                self.assertEqual(result["overall"], "inconclusive")

    def test_target_script_has_valid_posix_shell_syntax(self):
        completed = subprocess.run(
            ["/bin/sh", "-n", str(HIL_SCRIPT)],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)


if __name__ == "__main__":
    unittest.main()
