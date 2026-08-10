from pathlib import Path

from PIL import Image, ImageDraw, ImageFont
import pypdfium2 as pdfium


HERE = Path(__file__).resolve().parent
DATASHEET = HERE / "sources" / "Rockchip_RV1106_Datasheet_Rev1.8.pdf"
OUT = HERE / "assets" / "manual"
OUT.mkdir(parents=True, exist_ok=True)


def render_page(page_index: int) -> Image.Image:
    document = pdfium.PdfDocument(str(DATASHEET))
    try:
        return document[page_index].render(scale=2.5).to_pil().convert("RGB")
    finally:
        document.close()


def crop(page_index: int, dst_name: str, box: tuple[int, int, int, int]) -> None:
    with render_page(page_index) as image:
        image.crop(box).save(OUT / dst_name, optimize=True)


crop(
    5,
    "rv1106-overview-rev18-p06.png",
    (88, 48, 1328, 975),
)
crop(
    13,
    "rv1106-block-diagram-rev18-p14.png",
    (92, 48, 1328, 1285),
)

with Image.open(OUT / "rv1106-hardware-guide-v11-page11-screenshot.png") as image:
    image.crop((55, 505, 790, 1095)).save(
        OUT / "rv1106-hardware-guide-v11-page11-rv1106-crop.png",
        optimize=True,
    )


W, H = 1600, 830
card = Image.new("RGB", (W, H), "white")
draw = ImageDraw.Draw(card)
font_dir = Path("C:/Windows/Fonts")
title_font = ImageFont.truetype(str(font_dir / "msyhbd.ttc"), 48)
body_font = ImageFont.truetype(str(font_dir / "msyh.ttc"), 38)
small_font = ImageFont.truetype(str(font_dir / "msyh.ttc"), 28)
mono_font = ImageFont.truetype(str(font_dir / "consola.ttf"), 35)

blue = "#174EA6"
dark = "#1F2937"
muted = "#5F6B7A"
line = "#D6E2F5"
fill = "#F5F8FE"

draw.rounded_rectangle((18, 18, W - 18, H - 18), radius=22, outline=blue, width=4, fill="white")
draw.rectangle((18, 18, W - 18, 112), fill=blue)
draw.text((58, 38), "RV1106G 上电要求：先看手册，再读原理图", font=title_font, fill="white")

draw.text(
    (58, 142),
    "依据：瑞芯微《RV1103G/RV1106G 硬件设计指南》V1.1，第 10–11 页，2.2.1 节与图 2-6",
    font=small_font,
    fill=muted,
)

rows = [
    ("电压先后", "同一模块先建立低电压，再建立高电压"),
    ("阶段间隔", "相邻的不同上电阶段应留出大于 1 ms 的间隔"),
    ("软启动", "电源器件的软启动时间应大于 100 μs"),
]

y = 215
for label, value in rows:
    draw.rounded_rectangle((55, y, W - 55, y + 116), radius=14, fill=fill, outline=line, width=2)
    draw.text((82, y + 31), label, font=body_font, fill=blue)
    draw.text((315, y + 31), value, font=body_font, fill=dark)
    y += 132

draw.rounded_rectangle((55, y + 4, W - 55, y + 139), radius=14, fill="#EAF1FF", outline=blue, width=3)
draw.text((82, y + 37), "推荐顺序", font=body_font, fill=blue)
draw.text(
    (315, y + 39),
    "VDD_0V9 / VDD_ARM  ->  VCC_1V8 / VCC_DDR  ->  VCC_3V3",
    font=mono_font,
    fill=dark,
)

draw.text(
    (58, H - 67),
    "说明：本图是按手册原文整理的教学摘录，不代替原始手册；最终是否满足时序仍要用示波器测量。",
    font=small_font,
    fill=muted,
)

card.save(OUT / "rv1106-power-sequence-guide-v11-p10-11.png", optimize=True)
