from pathlib import Path

from PIL import Image, ImageChops
import pypdfium2 as pdfium


HERE = Path(__file__).resolve().parent
SOURCE = HERE / "sources" / "TI_TS3USB3031_RevD.pdf"
OUT = HERE / "assets" / "manual"


def trim_white_margin(image: Image.Image, pad: int = 36) -> Image.Image:
    rgb = image.convert("RGB")
    background = Image.new("RGB", rgb.size, "white")
    diff = ImageChops.difference(rgb, background).convert("L")
    diff = diff.point(lambda value: 255 if value > 12 else 0)
    bbox = diff.getbbox()
    if not bbox:
        return rgb
    left = max(0, bbox[0] - pad)
    top = max(0, bbox[1] - pad)
    right = min(rgb.width, bbox[2] + pad)
    bottom = min(rgb.height, bbox[3] + pad)
    return rgb.crop((left, top, right, bottom))


def render(page_number: int, scale: float = 4.0) -> Image.Image:
    document = pdfium.PdfDocument(str(SOURCE))
    try:
        return document[page_number - 1].render(scale=scale).to_pil().convert("RGB")
    finally:
        document.close()


def main() -> None:
    if not SOURCE.exists():
        raise FileNotFoundError(SOURCE)
    OUT.mkdir(parents=True, exist_ok=True)

    page10 = render(10)
    # Rev.D page 10: overview, functional diagram, feature text and function table.
    block = trim_white_margin(page10.crop((170, 170, page10.width - 170, 3000)))
    block.save(OUT / "ti-ts3usb3031-revd-p10-function-table.png", optimize=True)

    page15 = render(15)
    # Rev.D page 15: power recommendation and the first layout-guideline paragraphs.
    layout = trim_white_margin(page15.crop((170, 150, page15.width - 170, 2250)))
    layout.save(OUT / "ti-ts3usb3031-revd-p15-layout-guidelines.png", optimize=True)

    print("ti-ts3usb3031-revd-p10-function-table.png", block.size)
    print("ti-ts3usb3031-revd-p15-layout-guidelines.png", layout.size)


if __name__ == "__main__":
    main()
