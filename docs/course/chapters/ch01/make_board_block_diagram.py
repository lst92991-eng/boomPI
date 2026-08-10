from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import math


HERE = Path(__file__).resolve().parent
OUT_DIR = HERE / "assets" / "generated"
OUT_DIR.mkdir(parents=True, exist_ok=True)

W, H = 2800, 1460
BG = "#F7F8F4"
INK = "#263747"
LINE = "#3F6FC4"
SOC = "#F27A2A"
IFACE = "#416FBE"
DEVICE = "#6DAE45"
CONNECTOR = "#8BCB50"
USB = "#05A9C6"
POWER = "#D8A000"
WHITE = "#FFFFFF"
FONT_REG = Path(r"C:\Windows\Fonts\msyh.ttc")
FONT_BOLD = Path(r"C:\Windows\Fonts\msyhbd.ttc")


def font(size: int, bold: bool = False):
    return ImageFont.truetype(str(FONT_BOLD if bold else FONT_REG), size)


def canvas():
    image = Image.new("RGB", (W, H), BG)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((36, 36, W - 36, H - 36), 24, fill=WHITE, outline="#A9B4C0", width=4)
    return image, draw


def centered(draw, xy, text, size=40, fill=WHITE, bold=True):
    x1, y1, x2, y2 = xy
    f = font(size, bold)
    box = draw.multiline_textbbox((0, 0), text, font=f, spacing=9, align="center")
    tw, th = box[2] - box[0], box[3] - box[1]
    draw.multiline_text(
        ((x1 + x2 - tw) / 2, (y1 + y2 - th) / 2 - box[1]),
        text,
        font=f,
        fill=fill,
        spacing=9,
        align="center",
    )


def box(draw, xy, text, color, size=39, text_fill=WHITE):
    draw.rounded_rectangle(xy, 18, fill=color, outline="#FFFFFF", width=3)
    centered(draw, xy, text, size=size, fill=text_fill)


def arrow(draw, start, end, color=LINE, width=8, both=False):
    draw.line((start, end), fill=color, width=width)

    def head(tip, tail):
        tx, ty = tip
        px, py = tail
        angle = math.atan2(ty - py, tx - px)
        length = 28
        a = (tx + length * math.cos(angle + 2.58), ty + length * math.sin(angle + 2.58))
        b = (tx + length * math.cos(angle - 2.58), ty + length * math.sin(angle - 2.58))
        draw.polygon((tip, a, b), fill=color)

    head(end, start)
    if both:
        head(start, end)


def soc(draw, interfaces):
    body = (1030, 165, 1770, 1295)
    draw.rounded_rectangle(body, 28, fill=SOC, outline="#D65C16", width=5)
    draw.rectangle((1030, 165, 1770, 340), fill="#D65C16")
    centered(draw, (1030, 180, 1770, 300), "RV1106G3", 72)
    centered(draw, (1030, 290, 1770, 340), "主控 SoC", 30, fill="#FFF1E8", bold=False)
    for xy, label in interfaces:
        box(draw, xy, label, IFACE, size=31)
    return body


def make_core_storage_network():
    image, draw = canvas()
    interfaces = [
        ((1030, 430, 1325, 545), "时钟 / RTC"),
        ((1030, 635, 1325, 750), "eMMC"),
        ((1030, 840, 1325, 955), "SDMMC"),
        ((1475, 530, 1770, 645), "百兆网口"),
        ((1030, 1080, 1325, 1215), "电源 / 复位"),
    ]
    soc(draw, interfaces)

    left = [
        ((120, 390, 760, 570), "24 MHz 主时钟\n32.768 kHz 低速时钟", DEVICE, (760, 480), (1030, 480)),
        ((120, 610, 760, 790), "8 GB 标称容量 eMMC\n保存系统与文件", DEVICE, (760, 700), (1030, 700)),
        ((120, 830, 760, 1010), "microSD 卡座", CONNECTOR, (760, 920), (1030, 920)),
    ]
    for xy, label, color, start, end in left:
        box(draw, xy, label, color)
        arrow(draw, start, end, both=True)

    box(draw, (2050, 465, 2670, 695), "磁性器件与 RJ45\n100M 有线网络", USB)
    arrow(draw, (1770, 585), (2050, 585), both=True)

    box(draw, (120, 1100, 490, 1280), "Type-C\n5 V 输入", USB)
    box(draw, (610, 1100, 950, 1280), "保护与滤波", POWER)
    arrow(draw, (490, 1190), (610, 1190))
    arrow(draw, (950, 1190), (1030, 1145))
    box(draw, (1900, 1010, 2670, 1280), "四路降压电源\n0.9 V · 1.35 V · 1.8 V · 3.3 V\nRC 安排启动先后", POWER, size=36)
    arrow(draw, (1770, 1145), (1900, 1145))

    image.save(OUT_DIR / "boompi-core-storage-network-block-diagram.png", dpi=(240, 240), optimize=True)


def make_peripherals():
    image, draw = canvas()
    interfaces = [
        ((1030, 390, 1325, 505), "MIPI CSI"),
        ((1030, 590, 1325, 705), "SPI / I²C"),
        ((1475, 430, 1770, 545), "USB 2.0"),
        ((1475, 700, 1770, 835), "GPIO / 串口\nI²C / SPI"),
        ((1475, 1030, 1770, 1165), "音频接口"),
    ]
    soc(draw, interfaces)

    box(draw, (120, 325, 760, 530), "外接摄像头模组\n图像数据 + 控制信号", CONNECTOR)
    arrow(draw, (760, 430), (1030, 445), both=True)
    box(draw, (120, 570, 760, 790), "外接屏幕与触摸模组\n屏幕走 SPI · 触摸走 I²C", CONNECTOR, size=36)
    arrow(draw, (760, 680), (1030, 650), both=True)

    box(draw, (1960, 335, 2330, 600), "USB 3:1\n数据复用器", USB, size=40)
    arrow(draw, (1770, 485), (1960, 485), both=True)
    targets = [
        ((2450, 255, 2720, 400), "Type-C\n数据 / 烧录"),
        ((2450, 455, 2720, 600), "Wi-Fi\n无线模块"),
        ((2450, 655, 2720, 800), "USB-A\n外设"),
    ]
    for xy, label in targets:
        box(draw, xy, label, USB, size=34)
    draw.line(((2330, 485), (2390, 485), (2390, 325), (2450, 325)), fill=LINE, width=8)
    draw.line(((2330, 485), (2450, 525)), fill=LINE, width=8)
    draw.line(((2330, 485), (2390, 485), (2390, 725), (2450, 725)), fill=LINE, width=8)

    box(draw, (1960, 860, 2720, 1035), "2×20 扩展接口\n电源与通用信号", CONNECTOR)
    arrow(draw, (1770, 770), (1960, 945), both=True)
    box(draw, (2050, 1000, 2720, 1135), "麦克风接口", POWER, size=37)
    box(draw, (2050, 1190, 2720, 1325), "功放与扬声器接口", POWER, size=35)
    arrow(draw, (1770, 1080), (2050, 1068), both=True)
    draw.line(((1770, 1120), (1900, 1120), (1900, 1258)), fill=LINE, width=8)
    arrow(draw, (1900, 1258), (2050, 1258))

    image.save(OUT_DIR / "boompi-peripheral-block-diagram.png", dpi=(240, 240), optimize=True)


if __name__ == "__main__":
    make_core_storage_network()
    make_peripherals()
    print(OUT_DIR / "boompi-core-storage-network-block-diagram.png")
    print(OUT_DIR / "boompi-peripheral-block-diagram.png")
