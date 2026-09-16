"""Measure fixed before/after board-client responsibilities, with headers included."""
import argparse
import json
from pathlib import Path
from collections import defaultdict
from measure_client import measure

BASELINE = "2ca0cf63d241a032292f4d83a3120002b91a066e"
ROOT = Path(__file__).resolve().parents[1]


def group(path):
    name = Path(path).name
    if "/apps/boompi_client/" in path:
        return "入口与CLI"
    if "/application/" in path:
        return "问答应用"
    if "/config/" in path:
        return "配置与设备身份"
    if name in {"network_setup.cpp", "network_setup.h", "network.cpp", "network.h"}:
        return "网卡准备、发现与配置"
    if "/network/" in path:
        return "WSS与协议"
    if name in {"camera_capture.cpp", "camera_capture.h"}:
        return "摄像头预览"
    if name in {"display_touch.cpp", "display_touch.h"}:
        return "显示与触摸"
    if name in {"lvgl_screen.cpp", "lvgl_screen.h"}:
        return "LVGL页面"
    if "/ui/" in path:
        return "UI运行与显示交接"
    if "/audio/" in path or "/platform/rv1106/" in path:
        return "音频（含相关头文件）"
    raise ValueError("Unclassified source: " + path)


def count(ref):
    result = measure(ROOT, ref)
    modules = defaultdict(lambda: {"physical": 0, "eloc": 0, "files": 0})
    for file in result["by_file"]:
        row = modules[group(file["file"])]
        row["files"] += 1
        for key in ("physical", "eloc"):
            row[key] += file[key]
    return {"files": result["files"], "physical": result["physical"],
            "eloc": result["eloc"], "modules": dict(modules), "by_file": result["by_file"]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", default=BASELINE)
    parser.add_argument("--after", help="Default: current working tree")
    args = parser.parse_args()
    print(json.dumps({"before_ref": args.before, "after_ref": args.after or "working tree",
                      "scope": "Board C/C++ incl. headers; excludes scripts/tests/assets/vendor/Go",
                      "before": count(args.before), "after": count(args.after)},
                     indent=2, ensure_ascii=False))
