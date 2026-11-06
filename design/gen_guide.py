#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 A 屏(400x300 黑白快刷)「已联网 · 下一步」信息引导整屏图。

为什么是图片而不是 LVGL 文本：
  这块 SSD1619 上实测 LVGL 中文正文几乎不出墨（黑底白字条能看见，20px 正文
  的细笔画刷不出来），欢迎页/配网页都是走编译期烘焙 PNG + LVGL 图片通道。
  引导页沿用同一路径，保证用户一定能看清「打开哪个网址、做什么」。

定位（与 B 屏分工）：
  - A 屏 = 信息引导：当前卡在哪一步、下一步要做什么（本文件）
  - B 屏 = 品牌欢迎页：logo + 社名 + 售后群（见 gen_welcome_bwr.py）
  两张图都烘焙进固件，未联网/未绑定/未配内容时都不依赖服务器。

输出：
  firmware/s3/main/boards/custom/ssd2683za-s3-epd42/guide.png
  design/guide_preview.png
  firmware/s3/main/boards/custom/ssd2683za-s3-epd42/guide_png.h
"""
import os
from PIL import Image, ImageDraw, ImageFont

W, H = 400, 300
FONT_DIR = "/c/Windows/Fonts"
SIMHEI = os.path.join(FONT_DIR, "simhei.ttf")
MSYH = os.path.join(FONT_DIR, "msyh.ttc")

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW_DIR = os.path.join(BASE, "firmware", "s3", "main", "boards", "custom",
                      "ssd2683za-s3-epd42")
OUT_FW = os.path.join(FW_DIR, "guide.png")
OUT_PRE = os.path.join(BASE, "design", "guide_preview.png")
OUT_H = os.path.join(FW_DIR, "guide_png.h")

BLACK, WHITE = 0, 255

TITLE = "设备已联网 · 下一步"
STEPS = [
    ("1", "打开 dualcn.xiaowuai.vip"),
    ("2", "输入屏幕上的设备绑定码"),
    ("3", "后台配置要显示的内容"),
    ("4", "保存后设备自动更新画面"),
]
PILL = "售后技术QQ群：65558220"


def draw_corner_ticks(d, inset=8, t=2, L=14):
    pts = [
        (inset, inset, 1, 1), (W - inset, inset, -1, 1),
        (inset, H - inset, 1, -1), (W - inset, H - inset, -1, -1),
    ]
    for cx, cy, dx, dy in pts:
        d.line([(cx, cy), (cx + dx * L, cy)], fill=BLACK, width=t)
        d.line([(cx, cy), (cx, cy + dy * L)], fill=BLACK, width=t)


def main():
    canvas = Image.new("L", (W, H), WHITE)
    d = ImageDraw.Draw(canvas)

    # 1) 顶部黑底白字标题条：整屏最强视觉锚点，先告诉用户「现在到哪一步了」
    d.rectangle([0, 0, W, 44], fill=BLACK)
    title_font = ImageFont.truetype(SIMHEI, 22)
    d.text((W // 2, 22), TITLE, font=title_font, fill=WHITE, anchor="mm")

    # 2) 步骤列表：编号放黑底方块里，正文 16px 雅黑（墨水屏上比细黑体更实）
    num_font = ImageFont.truetype(SIMHEI, 15)
    body_font = ImageFont.truetype(MSYH, 16)
    y = 60
    line_h = 40
    for num, text in STEPS:
        box = 22
        d.rectangle([18, y, 18 + box, y + box], fill=BLACK)
        d.text((18 + box // 2, y + box // 2), num, font=num_font,
               fill=WHITE, anchor="mm")
        d.text((18 + box + 12, y + box // 2), text, font=body_font,
               fill=BLACK, anchor="lm")
        y += line_h

    # 3) 底部药丸：售后群（唯一反相块，扫一眼就能记住）
    pill_font = ImageFont.truetype(SIMHEI, 15)
    tb = d.textbbox((0, 0), PILL, font=pill_font)
    tw, th = tb[2] - tb[0], tb[3] - tb[1]
    pad_x, pad_y = 18, 9
    pill_w, pill_h = tw + pad_x * 2, th + pad_y * 2
    pill_x = (W - pill_w) // 2
    pill_y = H - pill_h - 12
    d.rounded_rectangle([pill_x, pill_y, pill_x + pill_w, pill_y + pill_h],
                        radius=min(12, pill_h // 2), fill=BLACK)
    d.text((W // 2, pill_y + pill_h // 2), PILL, font=pill_font,
           fill=WHITE, anchor="mm")

    # 4) 四角 L 形装饰（与欢迎页同一套语言，让两张图看起来是同一产品）
    draw_corner_ticks(d)

    canvas_rgb = canvas.convert("RGB")
    canvas_rgb.save(OUT_FW)
    canvas_rgb.save(OUT_PRE)

    data = open(OUT_FW, "rb").read()
    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write("// Auto-generated from guide.png by design/gen_guide.py. DO NOT EDIT.\n")
        f.write("#pragma once\n#include <cstddef>\n#include <stdint.h>\n\n")
        f.write("static const uint8_t guide_png[] = {\n")
        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            f.write("  " + ",".join("0x%02x" % b for b in chunk) + ",\n")
        f.write("};\n")
        f.write("static const size_t guide_png_len = %d;\n" % len(data))

    print("saved:", OUT_FW, canvas_rgb.size, "| header bytes", len(data))


if __name__ == "__main__":
    main()
