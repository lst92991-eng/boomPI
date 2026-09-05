"""Run one teaching checkpoint against the existing production-code tests.

This is a sequence of labs in one source tree, not independent lesson snapshots.
The runner never configures an SDK, deploys to a board, or starts board hardware.
"""
import argparse
import json
import shlex
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AUDIO_CASES = (
    "sub-grace-jitter", "confirmed-gap", "short-tail", "bounded-close",
    "bounded-capture", "bounded-command", "reset-preserves-pcm", "queue-results",
    "open-clears-error", "playback-clears-error", "playback-owner-order",
    "playback-prepare-failure", "drop-blocked-render", "drop-blocked-drain",
    "close-blocked-render", "voice-preroll", "voice-follow-up-boundary",
    "voice-barge-lifecycle",
)
ALL_TESTS = (
    "voice-client-config-contract", "protocol-json-contract",
    "voice-transport-loopback", "voice-client-behavior",
) + tuple("audio-engine-" + name for name in AUDIO_CASES)

# A checkpoint reuses real targets; it does not build an alternative product.
LESSONS = {
    1: ("配置与启动边界", ("boompi_voice_client_config_test",),
        ("voice-client-config-contract",)),
    2: ("固定音频帧与协议", ("boompi_protocol_json_test",),
        ("protocol-json-contract",)),
    3: ("播放队列与线程生命周期", ("boompi_audio_engine_harness",),
        tuple("audio-engine-" + name for name in (
            "queue-results", "short-tail", "playback-owner-order",
            "bounded-capture", "bounded-command", "bounded-close"))),
    4: ("开口、句首与追问输入", ("boompi_audio_engine_harness",),
        ("audio-engine-voice-preroll", "audio-engine-voice-follow-up-boundary")),
    5: ("真实本机WSS连接", ("boompi_voice_transport_loopback_test",),
        ("voice-transport-loopback",)),
    6: ("六态问答与异常路径", ("boompi_voice_client_harness",),
        ("voice-client-behavior",)),
    7: ("播报中插话与退出", ("boompi_audio_engine_harness",),
        ("audio-engine-voice-barge-lifecycle", "audio-engine-drop-blocked-render",
         "audio-engine-drop-blocked-drain", "audio-engine-close-blocked-render")),
    8: ("页面与板级显示端口", ("boompi_ui_simulator", "boompi_device_ui_compile"), ()),
    9: ("完整Host回归", (), ALL_TESTS),
}


def commands(step, build_dir, jobs):
    _, targets, tests = LESSONS[step]
    build = ["cmake", "--build", str(build_dir), "--config", "Debug"]
    if targets:
        build += ["--target", *targets]
    build += ["--parallel", str(jobs)]
    result = [build]
    if tests:
        # Names are a fixed allowlist, not shell input or user regexes.
        pattern = "^(" + "|".join(tests) + ")$"
        result.append([
            "ctest", "--test-dir", str(build_dir), "-C", "Debug",
            "--output-on-failure", "--no-tests=error", "-R", pattern,
        ])
    return result


def require_registered_tests(build_dir, required):
    result = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "-C", "Debug", "--show-only=json-v1"],
        check=True, capture_output=True, text=True,
    )
    registered = {test["name"] for test in json.loads(result.stdout)["tests"]}
    missing = sorted(set(required) - registered)
    if missing:
        raise RuntimeError(
            "未登记所需测试：" + ", ".join(missing) +
            "。检查Linux Host配置及OpenSSL/Boost/cJSON依赖；未执行不能视为通过。"
        )


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("step", nargs="?", type=int, choices=LESSONS)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/lesson-host")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--dry-run", action="store_true", help="仅打印命令，不执行")
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.step is None:
        for number, (title, _, _) in LESSONS.items():
            print(f"{number:02d}  {title}")
        return 0
    build_dir = args.build_dir.resolve()
    title, _, tests = LESSONS[args.step]
    print(f"第{args.step:02d}关：{title}", flush=True)
    plan = commands(args.step, build_dir, args.jobs)
    for command in plan:
        print("+ " + shlex.join(command), flush=True)
    if args.dry_run:
        return 0
    try:
        if not (build_dir / "CMakeCache.txt").is_file():
            raise RuntimeError("构建目录尚未配置；先按docs/teaching/README.md准备Host环境。")
        subprocess.run(plan[0], cwd=ROOT, check=True)
        if tests:
            require_registered_tests(build_dir, tests)
            subprocess.run(plan[1], cwd=ROOT, check=True)
        else:
            print("UI目标编译通过；页面外观、触摸和真实设备行为尚未验证。")
        print("本关Host检查结束；不代表已经通过真板验收。")
        return 0
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print("检查未通过：" + str(error))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
