from pathlib import Path
import sys

from PIL import Image, ImageDraw, ImageFont


def main() -> None:
    page_dir = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    files = sorted(page_dir.glob("page-*.png"))
    thumb_w, thumb_h = 360, 510
    cols = 4
    rows = (len(files) + cols - 1) // cols
    font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 22)
    canvas = Image.new("RGB", (cols * thumb_w, rows * (thumb_h + 34)), "#d9dee5")
    draw = ImageDraw.Draw(canvas)
    for index, source in enumerate(files):
        image = Image.open(source).convert("RGB")
        image.thumbnail((thumb_w - 12, thumb_h - 12), Image.Resampling.LANCZOS)
        row, col = divmod(index, cols)
        x = col * thumb_w + (thumb_w - image.width) // 2
        y = row * (thumb_h + 34) + 6
        canvas.paste(image, (x, y))
        draw.text((col * thumb_w + 10, row * (thumb_h + 34) + thumb_h + 4), f"P{index + 1:02d}", font=font, fill="#1f2b3a")
    output.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(output, optimize=True)
    print(output)


if __name__ == "__main__":
    main()
