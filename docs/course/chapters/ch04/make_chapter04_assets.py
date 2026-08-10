from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
OUT = HERE / "assets" / "concept"
FONT_DIR = Path("C:/Windows/Fonts")
FONT = ImageFont.truetype(str(FONT_DIR / "msyh.ttc"), 32)
FONT_SMALL = ImageFont.truetype(str(FONT_DIR / "msyh.ttc"), 27)
FONT_BOLD = ImageFont.truetype(str(FONT_DIR / "msyhbd.ttc"), 36)
FONT_TITLE = ImageFont.truetype(str(FONT_DIR / "msyhbd.ttc"), 46)
INK = "#20304A"
BLUE, BLUE_LIGHT = "#2F6FED", "#EAF1FF"
GREEN, GREEN_LIGHT = "#2C8C69", "#E8F6F0"
ORANGE, ORANGE_LIGHT = "#D9792B", "#FFF1E5"
GRAY, GRAY_LIGHT = "#61718B", "#F3F5F8"


def canvas(title, subtitle="", size=(1800, 900)):
    image = Image.new("RGB", size, "white")
    draw = ImageDraw.Draw(image)
    draw.text((80, 45), title, font=FONT_TITLE, fill=INK)
    if subtitle:
        draw.text((82, 110), subtitle, font=FONT_SMALL, fill=GRAY)
    return image, draw


def box(draw, xy, text, fill, outline, font=FONT):
    draw.rounded_rectangle(xy, radius=22, fill=fill, outline=outline, width=4)
    left, top, right, bottom = xy
    bounds = draw.multiline_textbbox((0, 0), text, font=font, spacing=10, align="center")
    x = left + (right - left - (bounds[2] - bounds[0])) / 2
    y = top + (bottom - top - (bounds[3] - bounds[1])) / 2
    draw.multiline_text((x, y), text, font=font, fill=INK, spacing=10, align="center")


def arrow(draw, start, end, fill=GRAY):
    draw.line((start, end), fill=fill, width=6)
    x, y = end
    draw.polygon([(x, y), (x - 22, y - 14), (x - 22, y + 14)], fill=fill)


def save(image, name):
    OUT.mkdir(parents=True, exist_ok=True)
    image.save(OUT / name, optimize=True)


def make_relation():
    image, draw = canvas("开发电脑与开发板不是同一个Linux环境", "Buildroot在Ubuntu主机上构建；生成的系统镜像交给boomPI运行")
    box(draw, (100, 220, 700, 690), "开发电脑\nUbuntu 26.04 LTS\nx86_64\n\n运行Buildroot\n交叉编译与制作镜像", BLUE_LIGHT, BLUE)
    box(draw, (1100, 220, 1700, 690), "boomPI开发板\nLinux 5.10.160\narmv7l\n\n运行生成后的\n内核与根文件系统", GREEN_LIGHT, GREEN)
    arrow(draw, (700, 455), (1100, 455), ORANGE)
    draw.text((830, 410), "系统镜像", font=FONT_BOLD, fill=ORANGE)
    save(image, "linux-ubuntu-buildroot.png")


def make_tree():
    image = Image.new("RGB", (1800, 520), "white")
    draw = ImageDraw.Draw(image)
    box(draw, (760, 30, 1040, 140), "/\n根目录", BLUE_LIGHT, BLUE, FONT_BOLD)
    entries = [(190, "/etc\n系统配置", BLUE_LIGHT, BLUE), (470, "/bin  /sbin\n基础命令", BLUE_LIGHT, BLUE), (750, "/proc  /sys\n内核运行信息", GREEN_LIGHT, GREEN), (1030, "/dev\n设备入口", GREEN_LIGHT, GREEN), (1310, "/tmp  /run\n临时数据", ORANGE_LIGHT, ORANGE)]
    draw.line((900, 140, 900, 230), fill=GRAY, width=5)
    draw.line((310, 230, 1430, 230), fill=GRAY, width=5)
    for x, label, fill, outline in entries:
        draw.line((x + 120, 230, x + 120, 300), fill=GRAY, width=4)
        box(draw, (x, 300, x + 240, 490), label, fill, outline, FONT_SMALL)
    save(image, "board-directory-tree.png")


def main():
    make_relation()
    make_tree()
    for name in ("linux-ubuntu-buildroot.png", "board-directory-tree.png"):
        path = OUT / name
        print(path.name, path.stat().st_size)


if __name__ == "__main__":
    main()
