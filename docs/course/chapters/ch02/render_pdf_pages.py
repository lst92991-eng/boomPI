from pathlib import Path
import sys

import pypdfium2 as pdfium


def main() -> None:
    source = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    document = pdfium.PdfDocument(str(source))
    for index in range(len(document)):
        image = document[index].render(scale=2.2).to_pil().convert("RGB")
        image.save(output / f"page-{index + 1:03d}.png", optimize=True)
    print(f"rendered {len(document)} pages to {output}")


if __name__ == "__main__":
    main()
