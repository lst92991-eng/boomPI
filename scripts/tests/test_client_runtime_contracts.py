"""Check the real supervisor in a temporary directory; never touch a board/network."""
import os
import signal
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class ClientRuntimeContracts(unittest.TestCase):
    def test_no_ap_path_remains(self):
        script = (ROOT / 'client/scripts/boompi-clientctl').read_text(encoding="utf-8")
        self.assertNotIn('needs_provision', script)
        self.assertNotIn('boompi-provision', script)
        self.assertNotIn('--save-wifi', (ROOT / 'client/apps/boompi_client/main.cpp').read_text(encoding="utf-8"))
        self.assertFalse((ROOT / 'client/scripts/boompi-provision').exists())

    def test_camera_keeps_board_format(self):
        text = (ROOT / 'client/src/ui/camera_capture.cpp').read_text(encoding="utf-8")
        command = text[text.index('constexpr char kCameraCommand'):text.index('std::thread worker')]
        self.assertIn('-framerate 25', command)
        self.assertIn('fps=5,scale=320:180', command)
        self.assertNotIn('--set-parm', command)

    @unittest.skipUnless(os.name == 'posix' and Path('/proc/self/cmdline').exists(), 'Linux supervisor')
    def test_start_stop_orphan_and_update_rollback(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            base = root / 'data'
            run = root / 'run'
            run.mkdir()
            (base / 'bin').mkdir(parents=True)
            app = base / 'bin/boompi-client'
            good = '#!/usr/bin/python3\nimport sys,time\nif "--check-config" in sys.argv: sys.exit(0)\nwhile True: time.sleep(0.1)\n'
            app.write_text(good, encoding="utf-8")
            app.chmod(0o755)
            script = (ROOT / 'client/scripts/boompi-clientctl').read_text(encoding="utf-8")
            control = root / 'boompi-clientctl'
            # Keep host loader paths: board libraries must not be injected into host tools.
            script = script.replace('LD_LIBRARY_PATH=/oem/usr/lib:/usr/lib:/lib', 'LD_LIBRARY_PATH=')
            script = script.replace('BASE=/userdata/boompi', f'BASE={base}')
            script = script.replace('/run/boompi-', str(run / 'boompi-'))
            script = script.replace('SELF=/usr/sbin/boompi-clientctl', f'SELF={control}')
            control.write_text(script, encoding="utf-8")
            control.chmod(0o755)
            def command(*args):
                return subprocess.run([str(control), *args], text=True, capture_output=True, timeout=40)
            def owned(pid):
                try:
                    return str(app).encode() in Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
                except FileNotFoundError:
                    return False
            try:
                started = command('start')
                self.assertEqual(started.returncode, 0, started.stderr)
                child = int((run / 'boompi-client-child.pid').read_text(encoding="utf-8"))
                self.assertTrue(owned(child))
                supervisor = int((run / 'boompi-client.pid').read_text(encoding="utf-8"))
                os.kill(supervisor, signal.SIGKILL)
                time.sleep(0.1)
                self.assertEqual(command('stop').returncode, 0)
                self.assertFalse(owned(child))
                started = command('start')
                self.assertEqual(started.returncode, 0, started.stderr)
                replacement = root / 'bad-client'
                replacement.write_text('#!/bin/sh\n[ "$1" = --check-config ] && exit 0\nexit 1\n', encoding="utf-8")
                replacement.chmod(0o755)
                update = command('update', str(replacement))
                self.assertNotEqual(update.returncode, 0)
                self.assertIn('previous version restored', update.stderr)
                self.assertEqual(app.read_text(encoding="utf-8"), good)
                self.assertIn('is running', command('status').stdout)
                self.assertEqual(command('stop').returncode, 0)
                self.assertIn('is stopped', command('status').stdout)
            finally:
                command('stop')
