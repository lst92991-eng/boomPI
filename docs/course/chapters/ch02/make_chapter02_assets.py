from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont
import pypdfium2 as pdfium


HERE = Path(__file__).resolve().parent
SCHEMATIC = HERE.parent / "ch01" / "sources" / "SCH_Schematic1_2026-08-10.pdf"
OUT = HERE / "assets" / "schematic-clean"
QA_OUT = HERE / "qa" / "source-pages"

# Crop coordinates come from direct page renders.  The source PDF mixes A3 and
# A4 pages, so each page keeps its own reference size instead of sharing one
# global scale.
PAGE_REFERENCE_SIZE = {
    1: (2996, 2123),
    2: (4225, 2990),
    3: (2996, 2123),
    4: (2996, 2123),
    5: (2123, 1502),
    6: (2996, 2123),
    7: (2123, 1502),
    8: (2123, 1502),
    9: (2123, 1502),
    10: (2123, 1502),
    11: (2123, 1502),
    12: (2123, 1502),
}

# Each output is a clean module crop.  No red box, arrow, number, legend or
# teaching overlay is added to the original schematic pixels.
CROPS = {
    "p01-typec-interface.png": (1, (20, 20, 1210, 600)),
    "p01-input-protection.png": (1, (1225, 20, 675, 520)),
    "p01-wifi-3v3-supply.png": (1, (1960, 165, 1010, 380)),
    "p01-power-tree.png": (1, (20, 760, 1260, 470)),
    "p01-power-sequence-rc.png": (1, (1260, 380, 780, 340)),
    "p01-arm-0v9-filter.png": (1, (2040, 680, 930, 350)),
    "p01-rtc-backup.png": (1, (20, 1380, 780, 350)),
    "p01-out-3v3-supply.png": (1, (1400, 1270, 1100, 360)),
    "p01-typec-detect.png": (1, (940, 1580, 520, 500)),
    "p01-emmc-voltage-select.png": (1, (20, 1720, 950, 370)),
    "p02-main-clock-reset-debug.png": (2, (150, 70, 1800, 820)),
    "p02-main-core-interfaces.png": (2, (2450, 80, 1450, 1150)),
    "p02-touch-interfaces.png": (2, (1120, 1430, 880, 390)),
    "p02-display-interfaces.png": (2, (1120, 1800, 880, 370)),
    "p02-camera-mipi-interfaces.png": (2, (150, 2150, 1800, 600)),
    "p02-main-storage-recovery.png": (2, (2350, 1450, 1500, 1200)),
    "p03-m8800ds2-wireless.png": (3, (500, 300, 1550, 1250)),
    "p04-hr911105a-ethernet.png": (4, (900, 300, 1150, 900)),
    "p05-microsd-card.png": (5, (120, 180, 1250, 700)),
    "p06-emmc-device.png": (6, (220, 380, 1000, 620)),
    "p06-emmc-support.png": (6, (1250, 250, 900, 650)),
    "p08-fm8002a-amplifier.png": (8, (250, 130, 1350, 310)),
    "p08-mic0-connector.png": (8, (100, 620, 970, 500)),
    "p08-mic1-connector.png": (8, (1060, 620, 940, 500)),
    "p09-expansion-40pin.png": (9, (450, 130, 1200, 900)),
    "p10-camera-fpc.png": (10, (400, 220, 1200, 520)),
    "p11-usb-selector.png": (11, (80, 200, 650, 430)),
    "p11-ts3usb3031-mux.png": (11, (760, 180, 850, 440)),
    "p11-usba-connector.png": (11, (20, 560, 750, 480)),
    "p12-display-touch-fpc.png": (12, (450, 180, 1050, 800)),
}

PAGE_TITLES = {
    1: "供电与上电顺序",
    2: "RV1106G3 主控",
    3: "Wi-Fi / 蓝牙模块",
    4: "以太网接口",
    5: "microSD 卡座",
    6: "eMMC 存储",
    7: "空白占位页",
    8: "功放与麦克风接口",
    9: "40Pin 扩展接口",
    10: "摄像头接口",
    11: "USB 复用与 USB-A",
    12: "屏幕与触摸接口",
}


def trim_white_margin(image: Image.Image, pad: int = 42) -> Image.Image:
    rgb = image.convert("RGB")
    background = Image.new("RGB", rgb.size, "white")
    diff = ImageChops.difference(rgb, background).convert("L")
    diff = diff.point(lambda value: 255 if value > 10 else 0)
    bbox = diff.getbbox()
    if not bbox:
        return rgb
    left = max(0, bbox[0] - pad)
    top = max(0, bbox[1] - pad)
    right = min(rgb.width, bbox[2] + pad)
    bottom = min(rgb.height, bbox[3] + pad)
    return rgb.crop((left, top, right, bottom))


def make_page_map(pages: dict[int, Image.Image]) -> None:
    font_dir = Path("C:/Windows/Fonts")
    title_font = ImageFont.truetype(str(font_dir / "msyhbd.ttc"), 34)
    label_font = ImageFont.truetype(str(font_dir / "msyh.ttc"), 26)
    card_w, card_h = 620, 430
    gap_x, gap_y = 28, 38
    canvas = Image.new("RGB", (4 * card_w + 5 * gap_x, 3 * card_h + 4 * gap_y), "white")
    draw = ImageDraw.Draw(canvas)
    draw.text((gap_x, 8), "boomPI 原理图页码地图", font=title_font, fill="#20304A")
    for page_number in range(1, 13):
        row, col = divmod(page_number - 1, 4)
        x = gap_x + col * (card_w + gap_x)
        y = gap_y + 34 + row * (card_h + gap_y)
        page = pages[page_number].copy()
        page.thumbnail((card_w, card_h - 56), Image.Resampling.LANCZOS)
        px = x + (card_w - page.width) // 2
        py = y + 50 + (card_h - 56 - page.height) // 2
        canvas.paste(page, (px, py))
        draw.text(
            (x, y),
            f"P{page_number:02d}  {PAGE_TITLES[page_number]}",
            font=label_font,
            fill="#20304A",
        )
    canvas.save(OUT / "schematic-page-map.png", optimize=True)


def main() -> None:
    if not SCHEMATIC.exists():
        raise FileNotFoundError(SCHEMATIC)
    OUT.mkdir(parents=True, exist_ok=True)
    document = pdfium.PdfDocument(str(SCHEMATIC))
    pages = {
        page_number: document[page_number - 1].render(scale=5).to_pil().convert("RGB")
        for page_number in range(1, 13)
    }
    QA_OUT.mkdir(parents=True, exist_ok=True)
    for page_number in (1, 2, 3, 6, 8, 9, 10, 11, 12):
        pages[page_number].save(QA_OUT / f"page-{page_number:02d}.png", optimize=True)

    for filename, (page_number, crop) in CROPS.items():
        image = pages[page_number]
        ref_w, ref_h = PAGE_REFERENCE_SIZE[page_number]
        scale_x = image.width / ref_w
        scale_y = image.height / ref_h
        x, y, width, height = crop
        module = image.crop(
            (
                round(x * scale_x),
                round(y * scale_y),
                round((x + width) * scale_x),
                round((y + height) * scale_y),
            )
        )
        module = trim_white_margin(module)
        module.save(OUT / filename, optimize=True)
        print(filename, module.size)

    make_page_map(pages)
    print("schematic-page-map.png")


if __name__ == "__main__":
    main()
