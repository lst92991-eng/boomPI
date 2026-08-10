from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


HERE = Path(__file__).resolve().parent
OUT = HERE / "assets" / "concept"
FONT_DIR = Path("C:/Windows/Fonts")
FONT = ImageFont.truetype(str(FONT_DIR / "msyh.ttc"), 34)
FONT_SMALL = ImageFont.truetype(str(FONT_DIR / "msyh.ttc"), 28)
FONT_BOLD = ImageFont.truetype(str(FONT_DIR / "msyhbd.ttc"), 38)
FONT_TITLE = ImageFont.truetype(str(FONT_DIR / "msyhbd.ttc"), 48)

INK = "#20304A"
BLUE = "#2F6FED"
BLUE_LIGHT = "#EAF1FF"
GREEN = "#2C8C69"
GREEN_LIGHT = "#E8F6F0"
ORANGE = "#D9792B"
ORANGE_LIGHT = "#FFF1E5"
GRAY = "#61718B"
GRAY_LIGHT = "#F3F5F8"
RED = "#C4473A"


def canvas(title: str, subtitle: str = ""):
    image = Image.new("RGB", (1800, 980), "white")
    draw = ImageDraw.Draw(image)
    draw.text((80, 48), title, font=FONT_TITLE, fill=INK)
    if subtitle:
        draw.text((82, 112), subtitle, font=FONT_SMALL, fill=GRAY)
    return image, draw


def box(draw, xy, text, *, fill=GRAY_LIGHT, outline=BLUE, font=FONT, radius=24):
    draw.rounded_rectangle(xy, radius=radius, fill=fill, outline=outline, width=4)
    left, top, right, bottom = xy
    bbox = draw.multiline_textbbox((0, 0), text, font=font, spacing=10, align="center")
    x = left + (right - left - (bbox[2] - bbox[0])) / 2
    y = top + (bottom - top - (bbox[3] - bbox[1])) / 2
    draw.multiline_text((x, y), text, font=font, fill=INK, spacing=10, align="center")


def arrow(draw, start, end, *, fill=BLUE, width=6):
    draw.line((start, end), fill=fill, width=width)
    x2, y2 = end
    x1, y1 = start
    if abs(x2 - x1) >= abs(y2 - y1):
        direction = 1 if x2 > x1 else -1
        points = [(x2, y2), (x2 - direction * 22, y2 - 14), (x2 - direction * 22, y2 + 14)]
    else:
        direction = 1 if y2 > y1 else -1
        points = [(x2, y2), (x2 - 14, y2 - direction * 22), (x2 + 14, y2 - direction * 22)]
    draw.polygon(points, fill=fill)


def save(image, name):
    OUT.mkdir(parents=True, exist_ok=True)
    image.save(OUT / name, optimize=True)


def make_stack():
    image, draw = canvas("应用怎样到达硬件", "应用提出请求，Linux内核安排资源，设备驱动完成具体操作")
    layers = [
        ((180, 190, 1620, 325), "应用程序：读取数据 · 保存文件 · 申请内存", BLUE_LIGHT, BLUE),
        ((180, 405, 1620, 565), "Linux内核：安排处理器 · 管理内存 · 组织文件", GREEN_LIGHT, GREEN),
        ((300, 650, 1500, 765), "设备驱动：把应用请求变成具体硬件操作", ORANGE_LIGHT, ORANGE),
        ((180, 845, 1620, 950), "硬件：RV1106G3 · eMMC · 网口 · 屏幕 · 摄像头", GRAY_LIGHT, GRAY),
    ]
    for xy, text, fill, outline in layers:
        box(draw, xy, text, fill=fill, outline=outline, font=FONT)
    for y1, y2 in ((325, 405), (565, 650), (765, 845)):
        arrow(draw, (840, y1), (840, y2), fill=BLUE, width=5)
        arrow(draw, (960, y2), (960, y1), fill=GREEN, width=5)
    save(image, "os-stack.png")


def make_scheduler():
    image, draw = canvas("一颗处理器怎样让多个任务向前推进", "操作系统轮流分配处理器时间；等待数据的任务先让出处理器")
    draw.text((90, 200), "CPU时间", font=FONT_BOLD, fill=INK)
    x0, y0, h = 330, 190, 115
    segments = [
        (0, 260, "任务A", BLUE_LIGHT, BLUE),
        (260, 500, "任务B", GREEN_LIGHT, GREEN),
        (500, 760, "任务A", BLUE_LIGHT, BLUE),
        (760, 1050, "任务C", ORANGE_LIGHT, ORANGE),
        (1050, 1380, "任务B", GREEN_LIGHT, GREEN),
    ]
    for start, end, label, fill, outline in segments:
        box(draw, (x0 + start, y0, x0 + end, y0 + h), label, fill=fill, outline=outline, font=FONT_SMALL, radius=10)
    arrow(draw, (330, 345), (1710, 345), fill=GRAY, width=4)
    draw.text((330, 365), "较早", font=FONT_SMALL, fill=GRAY)
    draw.text((1630, 365), "较晚", font=FONT_SMALL, fill=GRAY)

    rows = [
        (520, "任务A", "运行一段时间，让出处理器，稍后继续", BLUE),
        (655, "任务B", "等待数据时先让出处理器，数据到达后继续", GREEN),
        (790, "任务C", "有工作需要处理时，加入轮流运行", ORANGE),
    ]
    for y, name, desc, color in rows:
        draw.rounded_rectangle((150, y, 1650, y + 95), 18, fill="white", outline=color, width=4)
        draw.text((190, y + 23), name, font=FONT_BOLD, fill=color)
        draw.text((500, y + 27), desc, font=FONT_SMALL, fill=INK)
    save(image, "scheduler-timeline.png")


def make_boot():
    image = Image.new("RGB", (1800, 420), "white")
    draw = ImageDraw.Draw(image)
    items = [
        (35, "上电与复位\n硬件稳定", GRAY_LIGHT, GRAY),
        (325, "BootROM\n芯片内固定程序", ORANGE_LIGHT, ORANGE),
        (615, "引导程序\n装载内核", BLUE_LIGHT, BLUE),
        (905, "Linux内核\n管理硬件与任务", GREEN_LIGHT, GREEN),
        (1195, "根文件系统与init\n准备用户空间", ORANGE_LIGHT, ORANGE),
        (1485, "用户空间\n服务与应用", BLUE_LIGHT, BLUE),
    ]
    for x, text, fill, outline in items:
        box(draw, (x, 75, x + 250, 335), text, fill=fill, outline=outline, font=FONT_SMALL)
    for x in (285, 575, 865, 1155, 1445):
        arrow(draw, (x, 205), (x + 40, 205), fill=GRAY, width=5)
    save(image, "boot-chain.png")


def main():
    make_stack()
    make_scheduler()
    make_boot()
    for name in ("os-stack.png", "scheduler-timeline.png", "boot-chain.png"):
        path = OUT / name
        print(path.name, path.stat().st_size)


if __name__ == "__main__":
    main()
