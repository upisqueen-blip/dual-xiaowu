#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 A 屏(400x300 黑白快刷)「未联网配网指引」整屏图。

为什么用烘焙图片而不是 LVGL 文本：
  这块 SSD1619 快刷屏上实测 LVGL 的中文正文几乎不出墨（黑底白字条能看见，
  正文字号 20px 的细笔画基本刷不出来），而 compiled-in PNG（欢迎页）显示正常。
  所以配网指引同样走「编译期烘焙 PNG + LVGL 图片通道」这条路，保证用户一定能
  看清要连哪个热点、打开什么网址、激活码在哪输入。

设计：
  - 顶部黑底白字标题条（唯一的强对比锚点）
  - 5 个编号步骤，数字放黑底小方块里，正文 18px 黑字
  - 底部黑底白字药丸：提示配网成功后会自动继续
  - 热点名不写死（每台设备不同），改为「选 Xiaowu- 开头的热点」

输出：
  firmware/s3/main/boards/custom/ssd2683za-s3-epd42/provisioning.png
  design/provisioning_preview.png
  firmware/s3/main/boards/custom/ssd2683za-s3-epd42/provisioning_png.h
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
OUT_FW = os.path.join(FW_DIR, "provisioning.png")
OUT_PRE = os.path.join(BASE, "design", "provisioning_preview.png")
OUT_H = os.path.join(FW_DIR, "provisioning_png.h")

BLACK, WHITE = 0, 255

STEPS = [
    ("1", "手机 WiFi 选 Xiaowu- 热点"),
    ("2", "浏览器打开 192.168.4.1"),
    ("3", "选家里 WiFi 并输密码"),
    ("4", "屏幕显示 6 位激活码"),
    ("5", "xiaozhi.me 输入激活码"),
]


def main():
    canvas = Image.new("L", (W, H), WHITE)
    d = ImageDraw.Draw(canvas)

    # 1) 顶部标题条：黑底白字，整屏最强的视觉锚点
    d.rectangle([0, 0, W, 46], fill=BLACK)
    title_font = ImageFont.truetype(SIMHEI, 22)
    d.text((W // 2, 23), "设备未联网 · 配网指引", font=title_font,
           fill=WHITE, anchor="mm")

    # 2) 步骤列表
    num_font = ImageFont.truetype(SIMHEI, 15)
    body_font = ImageFont.truetype(MSYH, 18)
    y = 64
    line_h = 36
    for num, text in STEPS:
        box = 20
        d.rectangle([14, y, 14 + box, y + box], fill=BLACK)
        d.text((14 + box // 2, y + box // 2), num, font=num_font,
               fill=WHITE, anchor="mm")
        tx = 14 + box + 10
        d.text((tx, y + box // 2), text, font=body_font, fill=BLACK, anchor="lm")
        y += line_h

    # 3) 底部药丸：配网成功后自动继续
    pill_font = ImageFont.truetype(SIMHEI, 15)
    pill_text = "配网成功后自动继续，无需按键"
    tb = d.textbbox((0, 0), pill_text, font=pill_font)
    tw = tb[2] - tb[0]
    pad_x, pad_y = 16, 8
    pill_w = tw + pad_x * 2
    pill_h = (tb[3] - tb[1]) + pad_y * 2
    pill_x = (W - pill_w) // 2
    pill_y = H - pill_h - 12
    d.rounded_rectangle([pill_x, pill_y, pill_x + pill_w, pill_y + pill_h],
                        radius=pill_h // 2, fill=BLACK)
    d.text((W // 2, pill_y + pill_h // 2), pill_text, font=pill_font,
           fill=WHITE, anchor="mm")

    # 4) 纯黑白化：e-ink 驱动只认 0/255，避免抗锯齿灰阶变成脏点
    bw = canvas.point(lambda p: BLACK if p < 140 else WHITE)
    out_rgb = bw.convert("RGB")
    out_rgb.save(OUT_FW)
    out_rgb.save(OUT_PRE)

    data = open(OUT_FW, "rb").read()
    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write("// Auto-generated from provisioning.png by "
                "design/gen_provisioning.py. DO NOT EDIT.\n")
        f.write("#pragma once\n#include <cstddef>\n#include <stdint.h>\n\n")
        f.write("static const uint8_t provisioning_png[] = {\n")
        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            f.write("  " + ",".join("0x%02x" % b for b in chunk) + ",\n")
        f.write("};\n")
        f.write("static const size_t provisioning_png_len = %d;\n" % len(data))

    dark = sum(1 for p in bw.getdata() if p == BLACK)
    print("saved:", OUT_FW, out_rgb.size, "| header bytes", len(data),
          "| dark_pixels", dark)


if __name__ == "__main__":
    main()
