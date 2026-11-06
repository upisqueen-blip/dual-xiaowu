"""生成 5.83 寸双屏自检图案预览图。

用途：烧录前先让用户知道屏幕上应该依次出现什么，便于一眼判读。
输出：design/epd583_bringup_preview.png
"""
from PIL import Image, ImageDraw

W, H = 648, 480
SCALE = 0.42
GAP = 18
COLS = 2


def mono_patterns():
    """A 屏四张：全黑 / 全白 / 32px 棋盘 / 上半黑下半白+边框。"""
    out = []
    for name in ("全黑", "全白", "棋盘格 32px", "上半黑 / 下半白"):
        img = Image.new("1", (W, H), 1)
        d = ImageDraw.Draw(img)
        if name == "全黑":
            d.rectangle([0, 0, W, H], fill=0)
        elif name == "棋盘格 32px":
            for y in range(0, H, 32):
                for x in range(0, W, 32):
                    if ((x // 32) + (y // 32)) % 2 == 0:
                        d.rectangle([x, y, x + 31, y + 31], fill=0)
        elif name == "上半黑 / 下半白":
            d.rectangle([0, 0, W, H // 2], fill=0)
            d.rectangle([0, 0, W - 1, H - 1], outline=0, width=8)
        out.append((name, img))
    return out


def color_patterns():
    """B 屏三张：全白 / 全红 / 上黑中白下红三段。"""
    out = []
    for name in ("白底 + 红平面全 0x00", "白底 + 红平面全 0xFF", "三段横条 黑/白/红"):
        img = Image.new("RGB", (W, H), (255, 255, 255))
        d = ImageDraw.Draw(img)
        if name.endswith("0xFF"):
            d.rectangle([0, 0, W, H], fill=(200, 20, 30))
        elif name.startswith("三段"):
            d.rectangle([0, 0, W, H // 3], fill=(20, 20, 20))
            d.rectangle([0, (H * 2) // 3, W, H], fill=(200, 20, 30))
            d.rectangle([0, 0, W - 1, H - 1], outline=(20, 20, 20), width=12)
        out.append((name, img))
    return out


def main():
    mono = mono_patterns()
    color = color_patterns()
    tiles = mono + color
    scale = SCALE
    tw, th = int(W * scale), int(H * scale)
    rows = (len(tiles) + COLS - 1) // COLS
    canvas = Image.new("RGB", (COLS * tw + (COLS + 1) * GAP,
                               rows * (th + 34) + GAP + 40), (238, 240, 243))
    d = ImageDraw.Draw(canvas)
    d.text((GAP, 12), "5.83\" 双屏自检顺序：A 屏 4 张（黑白）→ B 屏 3 张（三色）",
           fill=(20, 20, 20))
    for i, (name, img) in enumerate(tiles):
        r, c = divmod(i, COLS)
        x = GAP + c * (tw + GAP)
        y = 40 + r * (th + 34)
        tile = img.convert("RGB").resize((tw, th), Image.LANCZOS)
        canvas.paste(tile, (x, y))
        d.rectangle([x, y, x + tw, y + th], outline=(120, 120, 120), width=1)
        tag = f"{i + 1}. {name}" if i < 4 else f"B{i - 3}. {name}"
        d.text((x, y + th + 8), tag, fill=(40, 40, 40))
    canvas.save("design/epd583_bringup_preview.png")
    print("saved design/epd583_bringup_preview.png", canvas.size)


if __name__ == "__main__":
    main()
