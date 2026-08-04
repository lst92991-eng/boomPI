#!/usr/bin/env python3
"""Convert the seven XiaoZhi Twemoji PNGs to LVGL 8 RGB565+alpha C assets."""

import argparse
from pathlib import Path

from PIL import Image


ASSETS = {
    "laughing": "1f606",
    "sad": "1f614",
    "surprised": "1f62f",
    "sleepy": "1f634",
    "neutral": "1f636",
    "happy": "1f642",
    "thinking": "1f914",
}


def convert(source: Path, output: Path, codepoint: str) -> None:
    image = Image.open(source).convert("RGBA")
    if image.size != (64, 64):
        raise ValueError(f"{source}: expected 64x64, got {image.size}")
    data = bytearray()
    for red, green, blue, alpha in image.getdata():
        rgb565 = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
        data.extend((rgb565 & 0xFF, rgb565 >> 8, alpha))
    rows = ["  " + ",".join(f"0x{byte:02x}" for byte in data[i:i + 24]) + ","
            for i in range(0, len(data), 24)]
    symbol = f"emoji_{codepoint}_64"
    output.write_text(
        "// Generated from the CC BY 4.0 Twemoji PNG; see NOTICE.md.\n"
        "#include <lvgl.h>\n\n"
        f"static const LV_ATTRIBUTE_MEM_ALIGN unsigned char {symbol}_map[] = {{\n"
        + "\n".join(rows)
        + "\n};\n\n"
        f"const lv_img_dsc_t {symbol} = {{\n"
        "  .header.always_zero = 0,\n"
        "  .header.reserved = 0,\n"
        "  .header.w = 64,\n"
        "  .header.h = 64,\n"
        "  .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,\n"
        f"  .data_size = sizeof({symbol}_map),\n"
        f"  .data = {symbol}_map,\n"
        "};\n",
        encoding="utf-8",
        newline="\n",
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name, codepoint in ASSETS.items():
        convert(args.source / f"{name}.png", args.output / f"emoji_{codepoint}_64.c", codepoint)


if __name__ == "__main__":
    main()
