#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 A 屏(400x300 黑白快刷)欢迎页 PNG：小悟 logo + 欢迎语 + 售后 QQ 群。

输出：
  firmware/s3/main/boards/custom/ssd2683za-s3-epd42/welcome.png  (烧入固件)
  design/welcome_preview.png                                     (预览)
  firmware/s3/main/boards/custom/ssd2683za-s3-epd42/welcome_png.h (C 数组，固件 include)
"""
import os
from PIL import Image, ImageDraw, ImageFont, ImageOps

W, H = 400, 300
FONT_DIR = "/c/Windows/Fonts"
SIMHEI = os.path.join(FONT_DIR, "simhei.ttf")      # 黑体，干净有力，做标题
MSYH   = os.path.join(FONT_DIR, "msyh.ttc")         # 雅黑，做副标题/提示
LOGO_SRC = r"E:\electron\screen\inksight-main\inksight-main\xiaowu-s3-open-platform\design\logo-print.png"
OUT_FW   = r"E:\electron\screen\inksight-main\inksight-main\xiaowu-s3-open-platform\firmware\s3\main\boards\custom\ssd2683za-s3-epd42\welcome.png"
OUT_PRE  = r"E:\electron\screen\inksight-main\inksight-main\xiaowu-s3-open-platform\design\welcome_preview.png"
OUT_H    = r"E:\electron\screen\inksight-main\inksight-main\xiaowu-s3-open-platform\firmware\s3\main\boards\custom\ssd2683za-s3-epd42\welcome_png.h"

BLACK, WHITE = 0, 255


def load_logo(target_w=330):
    """读取新 logo（黑底白描边的横向组合标：猴头水滴 + 小悟AI 字标）。
    反相为白底黑描边以适配墨水屏，裁掉黑边取紧致包围盒，按目标宽度等比
    缩放，返回 'L' 0/255 图（纯黑白，无灰阶抖动）。"""
    im = Image.open(LOGO_SRC).convert("L")
    # 原图：黑底(0) + 白描边(255)。反相 → 白底 + 黑描边
    im = ImageOps.invert(im)
    # 反相后黑描边即非白区域，取其紧致包围盒 = logo 内容
    bbox = im.getbbox()
    if bbox:
        im = im.crop(bbox)
    w, h = im.size
    scale = target_w / w
    nw, nh = max(1, round(w * scale)), max(1, round(h * scale))
    im = im.resize((nw, nh), Image.LANCZOS)
    # 阈值化（反相后描边=0、底色=255，已是纯黑白，这里再保险一次）
    im = im.point(lambda p: BLACK if p < 128 else WHITE)
    return im


def main():
    canvas = Image.new("L", (W, H), WHITE)
    d = ImageDraw.Draw(canvas)

    # 1) 细边框，卡片式专业感（内缩 5px，2px）
    d.rectangle([5, 5, W - 6, H - 6], outline=BLACK, width=2)

    # 2) Logo（猴头水滴 + 小悟AI 横向组合标）置顶居中
    logo = load_logo(330)
    lx = (W - logo.width) // 2
    ly = 14
    canvas.paste(logo, (lx, ly))

    # 3) 标题：欢迎加入小悟AI智研社
    title_font = ImageFont.truetype(SIMHEI, 28)
    d.text((W // 2, 158), "欢迎加入小悟AI智研社", font=title_font,
           fill=BLACK, anchor="mm")

    # 4) 分隔线（居中，140px 宽，2px）
    d.line([(W // 2 - 70, 180), (W // 2 + 70, 180)], fill=BLACK, width=2)

    # 5) 副标题：售后技术QQ群：65558220
    sub_font = ImageFont.truetype(MSYH, 19)
    d.text((W // 2, 204), "售后技术QQ群：65558220", font=sub_font,
           fill=BLACK, anchor="mm")

    # 6) 底部操作提示
    hint_font = ImageFont.truetype(MSYH, 13)
    d.text((W // 2, 270), "按 OK 键进入功能菜单", font=hint_font,
           fill=BLACK, anchor="mm")

    canvas_rgb = canvas.convert("RGB")  # 纯黑白 RGB，最通用，e-ink 驱动直接阈值化
    canvas_rgb.save(OUT_FW)
    canvas_rgb.save(OUT_PRE)

    # 同时导出 C 数组头文件，固件直接 #include，免去 EMBED_FILES 符号名不确定性
    data = open(OUT_FW, "rb").read()
    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write("// Auto-generated from welcome.png by design/gen_welcome.py. DO NOT EDIT.\n")
        f.write("#pragma once\n#include <cstddef>\n#include <stdint.h>\n\n")
        f.write("static const uint8_t welcome_png[] = {\n")
        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            f.write("  " + ",".join("0x%02x" % b for b in chunk) + ",\n")
        f.write("};\n")
        f.write("static const size_t welcome_png_len = %d;\n" % len(data))

    print("saved:", OUT_FW, "size", canvas_rgb.size, "mode", canvas_rgb.mode,
          "| header", OUT_H, "bytes", len(data))


if __name__ == "__main__":
    main()
