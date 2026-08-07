import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class ClientRuntimeContracts(unittest.TestCase):
    def read(self, relative_path):
        return (ROOT / relative_path).read_text(encoding="utf-8")

    def test_provision_owner_is_claimed_before_wifi_is_changed(self):
        script = self.read("client/scripts/boompi-provision")
        owner_call = script.index("\ntake_owner\n")
        wifi_mutation = script.index("\nkillall wpa_supplicant", owner_call)
        self.assertLess(owner_call, wifi_mutation)
        self.assertIn('[ "$OWNS_PROVISION" -eq 1 ] || return 0', script)
        self.assertIn("OWNER_LOCK=/run/boompi-provision.owner.lock", script)
        self.assertIn("flock -n 9", script)
        take_owner_definition = script.index("take_owner() {")
        acquire_call = script.index("acquire_owner_lock ||", take_owner_definition)
        self.assertLess(acquire_call, owner_call)

    def test_stop_cancels_an_orphaned_runtime_child(self):
        script = self.read("client/scripts/boompi-clientctl")
        stopped_branch = script.index("if ! running; then\n    cancel_saved_child")
        self.assertGreater(stopped_branch, script.index("stop() {"))
        self.assertIn("grep -qx '/usr/sbin/boompi-provision'", script)
        self.assertIn("setsid /usr/sbin/boompi-provision", script)
        self.assertIn('kill -TERM "-$SAVED_CHILD"', script)
        self.assertIn("CHILD_GROUP=/run/boompi-client-child.pgid", script)
        self.assertIn('provision_group_owned "$SAVED_CHILD"', script)
        self.assertIn("BOOMPI_PROVISION_OWNER=1 setsid", script)
        self.assertIn("boompi-provision --cleanup-stale", script)
        self.assertIn("LOCK=/run/boompi-clientctl.flock", script)
        self.assertIn('nohup "$0" _run 8>&-', script)
        self.assertIn('nohup "$0" _provision "$restore" 8>&-', script)
        handoff = script[script.index("run_provision() {"):script.index("launch_provision() {")]
        self.assertIn("if take_lock; then", handoff)
        self.assertLess(handoff.index("if take_lock; then"), handoff.index("start 1"))
        start = script[script.index("start() {"):script.index("stop() {")]
        self.assertIn("cancel_saved_child", start)

    def test_camera_source_and_display_are_capped_at_five_fps(self):
        source = self.read("client/src/ui/device_ui.cpp")
        command_start = source.index("constexpr char kCameraCommand[]")
        command_end = source.index(";", command_start)
        command = source[command_start:command_end]
        self.assertIn("constexpr unsigned kCameraFps = 5U;", source)
        self.assertNotIn("--set-parm", command)
        self.assertIn("不支持 VIDIOC_S_PARM", source)
        self.assertNotIn("-re -f rawvideo", source)
        self.assertIn("-framerate 25", source)
        self.assertIn("fps=5,scale=320:180", source)
        self.assertIn("if (camera_frame_ready) ++dropped_frames", source)

    def test_camera_error_discards_pixels_and_reports_load(self):
        device = self.read("client/src/ui/device_ui.cpp")
        screen = self.read("client/src/ui/lvgl_screen.cpp")
        error_start = device.index("void CameraError")
        error_end = device.index("bool TakeCameraFrame", error_start)
        error_path = device[error_start:error_end]
        self.assertIn("camera_frame_ready = false", error_path)
        self.assertIn("camera_frame.fill(0U)", error_path)
        self.assertIn("pipeline_fps=%u.%u", device)
        self.assertIn("load1=%.2f", device)
        self.assertIn("lv_obj_add_flag(camera_image, LV_OBJ_FLAG_HIDDEN)", screen)


if __name__ == "__main__":
    unittest.main()
