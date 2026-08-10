from pathlib import Path

from PIL import Image, ImageChops
import pypdfium2 as pdfium


HERE = Path(__file__).resolve().parent
SOURCE_PDF = HERE / "sources" / "SCH_Schematic1_2026-08-10.pdf"
OUTPUT = HERE / "assets" / "schematic-clean"
REFERENCE_SIZE = (2996, 2123)

# Coordinates are x, y, width, height on the direct PDF page renders.  Output is
# deliberately a clean crop: no boxes, arrows, numbers, legends, or added text.
CROPS = {
    "fig-1-02-rv1106-core-interfaces.png": (2, (2450, 80, 1450, 1050)),
    "fig-1-03-power-tree.png": (1, (40, 720, 1200, 550)),
    "fig-1-04-power-sequence.png": (1, (1300, 720, 720, 310)),
    "fig-1-05-ethernet-hr911105a.png": (4, (900, 300, 1150, 900)),
    "fig-1-06-wifi-bt-m8800ds2.png": (3, (450, 200, 1650, 1400)),
    "fig-1-07a-usb-selector.png": (11, (180, 360, 760, 480)),
    "fig-1-07b-usb-mux.png": (11, (1180, 300, 1050, 560)),
    "fig-1-08-emmc-voltage-select.png": (1, (100, 1800, 820, 260)),
    "fig-1-09-emmc-device.png": (6, (330, 430, 650, 335)),
    "fig-1-09b-emmc-support.png": (6, (1250, 250, 900, 650)),
    "fig-1-10-camera-fpc.png": (10, (450, 250, 1500, 850)),
    "fig-1-11-display-touch-fpc.png": (12, (450, 180, 1050, 700)),
    "fig-1-12-expansion-40pin.png": (9, (450, 130, 1200, 850)),
}


def trim_white_margin(image: Image.Image, pad: int = 24) -> Image.Image:
    rgb = image.convert("RGB")
    background = Image.new("RGB", rgb.size, (255, 255, 255))
    diff = ImageChops.difference(rgb, background)
    diff = diff.convert("L").point(lambda value: 255 if value > 10 else 0)
    bbox = diff.getbbox()
    if not bbox:
        return rgb
    left = max(0, bbox[0] - pad)
    top = max(0, bbox[1] - pad)
    right = min(rgb.width, bbox[2] + pad)
    bottom = min(rgb.height, bbox[3] + pad)
    return rgb.crop((left, top, right, bottom))


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    document = pdfium.PdfDocument(SOURCE_PDF)
    rendered = {}
    for filename, (page, crop) in CROPS.items():
        if page not in rendered:
            rendered[page] = document[page - 1].render(scale=8).to_pil().convert("RGB")
        image = rendered[page]
        scale_x = image.width / REFERENCE_SIZE[0]
        scale_y = image.height / REFERENCE_SIZE[1]
        x, y, width, height = crop
        module = image.crop((
            round(x * scale_x),
            round(y * scale_y),
            round((x + width) * scale_x),
            round((y + height) * scale_y),
        ))
        module = trim_white_margin(module, pad=54)
        module.save(OUTPUT / filename, optimize=True)
        print(filename, module.size)


if __name__ == "__main__":
    main()
