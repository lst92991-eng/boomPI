"""Measure the whole voice responsibility, including split files, headers and glue.

ALSA common setup is charged to capture; output functions are charged to playback.
Main includes the CLI and the input processing thread. VAD used to contain policy;
the VAD+speech subtotal prevents presenting that move as a deletion.
"""
import argparse
import json
import re
import subprocess
from pathlib import Path

from measure_client import source_metrics

ROOT = Path(__file__).resolve().parents[1]
GROUPS = {
    "ALSA capture + shared setup": [],
    "Rockchip 3A": ["src/platform/rv1106/rockchip_3a.cpp"],
    "Snowboy": ["src/platform/rv1106/wake.cpp"],
    "WebRTC VAD": ["src/platform/rv1106/vad.cpp"],
    "Speech + pre-roll + policy": ["src/audio/speech.cpp"],
    "WSS + protocol": ["src/network/voice_net.cpp", "src/network/voice_codec.cpp"],
    "Playback + ALSA output": ["src/audio/playback.cpp"],
    "Main + CLI + input thread": ["src/application/voice_client.cpp", "apps/boompi_client/main.cpp",
                                  "src/audio/audio_capture.cpp", "src/audio/voice_input.cpp"],
    "Snowboy ABI bridge": ["src/platform/rv1106/snowboy_legacy_bridge.cpp"],
    "Extra first-party glue": [],
    "Headers (including inline code)": [],
}
OUTPUT_FUNCTIONS = {"open_playback", "write", "prepare_playback", "drain", "drop",
                    "interrupt_playback", "playback_interrupted", "playback_error",
                    "clear_playback_error", "close_playback"}


def read_tree(revision):
    if revision:
        names = subprocess.check_output(["git", "ls-tree", "-r", "--name-only", revision,
                                         "--", "client"], cwd=ROOT, text=True).splitlines()
        read = lambda name: subprocess.check_output(["git", "show", f"{revision}:{name}"], cwd=ROOT).decode("utf-8")
    else:
        names = [p.relative_to(ROOT).as_posix() for p in (ROOT / "client").rglob("*") if p.is_file()]
        read = lambda name: (ROOT / name).read_text(encoding="utf-8")
    result = {}
    for name in names:
        if Path(name).suffix not in {".cpp", ".h", ".hpp", ".cc", ".c"}:
            continue
        if name.startswith(("client/tests/", "client/assets/", "client/cmake/",
                            "client/apps/boompi_ui_simulator/", "client/src/ui/")):
            continue
        if "display_touch" in name or "lvgl_screen" in name:
            continue
        if not name.startswith(("client/src/", "client/include/", "client/apps/boompi_client/")):
            continue
        result[name.removeprefix("client/")] = read(name)
    return result


def measure(revision=None):
    sources = read_tree(revision)
    rows = {name: {"physical": 0, "eloc": 0, "comment_only": 0, "sources": []} for name in GROUPS}

    def add(group, name, text):
        metrics = source_metrics(text)
        row = rows[group]
        row["physical"] += metrics["physical"]
        row["eloc"] += metrics["eloc"]
        row["comment_only"] += sum(bool(line.strip()) for line in text.splitlines()) - metrics["eloc"]
        row["sources"].append(name)

    if "src/platform/rv1106/audio_capture.cpp" in sources:
        add("ALSA capture + shared setup", "src/platform/rv1106/audio_capture.cpp",
            sources.pop("src/platform/rv1106/audio_capture.cpp"))
    else:
        alsa = sources.pop("src/platform/rv1106/alsa_audio.cpp")
        matches = list(re.finditer(r"^(?:bool|int|void|std::string) (\w+)\(", alsa, re.M))
        matches = matches[next(i for i, m in enumerate(matches) if m[1] == "open_capture"):]
        add("ALSA capture + shared setup", "alsa_audio.cpp:shared", alsa[:matches[0].start()])
        for i, match in enumerate(matches):
            end = matches[i + 1].start() if i + 1 < len(matches) else len(alsa)
            group = "Playback + ALSA output" if match[1] in OUTPUT_FUNCTIONS else "ALSA capture + shared setup"
            add(group, "alsa_audio.cpp:" + match[1], alsa[match.start():end])
    for group, names in GROUPS.items():
        for name in names:
            if name in sources:
                add(group, name, sources.pop(name))
    for name, text in sources.items():
        group = "Headers (including inline code)" if Path(name).suffix in {".h", ".hpp"} else "Extra first-party glue"
        add(group, name, text)
    core = list(GROUPS)[:8]
    total = lambda groups: {key: sum(rows[g][key] for g in groups) for key in ("physical", "eloc", "comment_only")}
    return {"modules": rows, "core_cpp": total(core), "all_voice": total(rows),
            "vad_and_policy": total(["WebRTC VAD", "Speech + pre-roll + policy"])}


def measure_audio(revision=None):
    """Audio implementation plus every direct header/adapter, excluding WSS and app/CLI."""
    stems = {"alsa_audio", "audio_capture", "audio_convert", "audio_thread",
             "board_voice_profile", "rockchip_3a", "wake", "vad", "snowboy_legacy_bridge"}
    files = {}
    for name, source in read_tree(revision).items():
        audio = name.startswith(("src/audio/", "include/boompi/audio/"))
        platform = name.startswith(("src/platform/rv1106/", "include/boompi/platform/rv1106/"))
        if audio or (platform and Path(name).stem in stems):
            files[name] = source_metrics(source)
    totals = {}
    for group, suffixes in {"cpp": {".cpp", ".cc", ".c"}, "headers": {".h", ".hpp"}}.items():
        selected = [m for name, m in files.items() if Path(name).suffix in suffixes]
        totals[group] = {key: sum(m[key] for m in selected) for key in ("physical", "eloc")}
    totals["total"] = {key: totals["cpp"][key] + totals["headers"][key] for key in ("physical", "eloc")}
    return {"totals": totals, "files": files}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", default="8018787795119011bf930f6afcb9c6203fbe07a4")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--audio", action="store_true", help="Only audio sources and their direct headers/adapters")
    args = parser.parse_args()
    if args.audio:
        before, after = measure_audio(args.before), measure_audio()
        if args.json:
            print(json.dumps({"before": before, "after": after}, indent=2, ensure_ascii=False))
        else:
            for group in ("cpp", "headers", "total"):
                print(group, before["totals"][group], "->", after["totals"][group])
        raise SystemExit(0)
    before, after = measure(args.before), measure()
    if args.json:
        print(json.dumps({"before": before, "after": after}, indent=2, ensure_ascii=False))
    else:
        print("Module | before physical/ELOC | after physical/ELOC")
        for name in GROUPS:
            old, new = before["modules"][name], after["modules"][name]
            print(f'{name} | {old["physical"]}/{old["eloc"]} | {new["physical"]}/{new["eloc"]}')
        for key in ("core_cpp", "all_voice", "vad_and_policy"):
            print(key, before[key], "->", after[key])
