import importlib.util
import contextlib
import io
import json
import subprocess
import unittest
from pathlib import Path
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "teaching_lab.py"
SPEC = importlib.util.spec_from_file_location("teaching_lab", SCRIPT)
LAB = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LAB)


class TeachingLabTests(unittest.TestCase):
    def test_all_checkpoints_have_commands_and_do_not_deploy(self):
        for step in LAB.LESSONS:
            commands = LAB.commands(step, Path("build/example"), 2)
            self.assertEqual(commands[0][:2], ["cmake", "--build"])
            self.assertTrue(all(command[0] in ("cmake", "ctest") for command in commands))
            if LAB.LESSONS[step][2]:
                self.assertIn("--no-tests=error", commands[1])

    def test_missing_tests_fail_instead_of_silently_skipping(self):
        result = subprocess.CompletedProcess([], 0, stdout=json.dumps({"tests": []}))
        with patch.object(LAB.subprocess, "run", return_value=result):
            with self.assertRaisesRegex(RuntimeError, "voice-transport-loopback"):
                LAB.require_registered_tests(Path("build/example"), ("voice-transport-loopback",))

    def test_registered_tests_are_accepted(self):
        result = subprocess.CompletedProcess([], 0, stdout=json.dumps({
            "tests": [{"name": name} for name in LAB.ALL_TESTS],
        }))
        with patch.object(LAB.subprocess, "run", return_value=result):
            LAB.require_registered_tests(Path("build/example"), LAB.ALL_TESTS)

    def test_dry_run_executes_nothing(self):
        with patch.object(LAB.subprocess, "run") as run, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(LAB.main(["7", "--dry-run"]), 0)
            run.assert_not_called()

    def test_checkpoints_match_current_ctest_registration(self):
        cmake = (SCRIPT.parents[1] / "client/tests/CMakeLists.txt").read_text(encoding="utf-8")
        for name in LAB.ALL_TESTS:
            self.assertIn(name.removeprefix("audio-engine-"), cmake)
        self.assertEqual(len(LAB.ALL_TESTS), len(set(LAB.ALL_TESTS)))


if __name__ == "__main__":
    unittest.main()
