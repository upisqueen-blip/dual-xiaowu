#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 A 屏(400x300 黑白快刷)欢迎页 PNG：小悟 logo + 欢迎语 + 售后 QQ 群。

输出：
  firmware/s3/main/boards/custom/ssd2683za-s3-epd42/welcome.png  (烧入固件)
  design/welcome_preview.png                                     (预览)
"""
import os
from PIL import Image, ImageDraw, ImageFont

W, H = 400, 300
FONT_DIR = "/c/Windows/Fonts"
SIMHEI = os.path.join(FONT_DIR, "simhei.ttf")      # 黑体，干净有力，做标题
MSYH   = os.path.join(FONT_DIR, "msyh.ttc")         # 雅黑，做副标题/提示
LOGO_SRC = r"E:\electron\screen\inksight-main\inksight-main\小悟logo.png"
OUT_FW   = r"E:\electron\screen\inksight-main\inksight-main\xiaowu-s3-open-platform\firmware\s3\main\boards\custom\ssd2683za-s3-epd42\welcome.png"
OUT_PRE  = r"E:\electron\screen\inksight-main\inksight-main\xiaowu-s3-open-platform\design\welcome_preview.png"
OUT_H    = r"E:\electron\screen\inksight-main\inksight-main\xiaowu-s3-open-platform\firmware\s3\main\boards\custom\ssd2683za-s3-epd42\welcome_png.h"

BLACK, WHITE = 0, 255


def load_logo(target_h=104):
    """读取 logo，裁掉白边取紧致包围盒，再取左侧正方形（仅水滴主标志，
    去掉右侧金色「小悟」字标，避免与欢迎语品牌字重复且视觉偏左），
    按目标高度等比缩放，返回 'L' 0/255 图。"""
    im = Image.open(LOGO_SRC).convert("L")
    bbox = im.getbbox()
    if bbox:
        im = im.crop(bbox)
    w, h = im.size
    # 只取左侧正方形 = 水滴主标志（字标在右，gold 已阈值化白）
    side = min(h, w)
    im = im.crop((0, 0, side, h))
    # 重新等比缩放到目标高度
    scale = target_h / h
    nw, nh = max(1, round(w * scale)), max(1, round(h * scale))
    im = im.resize((nw, nh), Image.LANCZOS)
    # 阈值：金/黑->黑(0)，白->白(255)；190 让金色描边保留为黑
    im = im.point(lambda p: BLACK if p < 190 else WHITE)
    return im


def main():
    canvas = Image.new("L", (W, H), WHITE)
    d = ImageDraw.Draw(canvas)

    # 1) 细边框，卡片式专业感（内缩 5px，2px）
    d.rectangle([5, 5, W - 6, H - 6], outline=BLACK, width=2)

    # 2) Logo 置顶居中
    logo = load_logo(104)
    lx = (W - logo.width) // 2
    ly = 20
    canvas.paste(logo, (lx, ly))

    # 3) 标题：欢迎加入小悟AI智研社
    title_font = ImageFont.truetype(SIMHEI, 30)
    d.text((W // 2, 158), "欢迎加入小悟AI智研社", font=title_font,
           fill=BLACK, anchor="mm")

    # 4) 分隔线（居中，140px 宽，2px）
    d.line([(W // 2 - 70, 180), (W // 2 + 70, 180)], fill=BLACK, width=2)

    # 5) 副标题：售后技术QQ群：65558220
    sub_font = ImageFont.truetype(MSYH, 20)
    d.text((W // 2, 204), "售后技术QQ群：65558220", font=sub_font,
           fill=BLACK, anchor="mm")

    # 6) 底部操作提示
    hint_font = ImageFont.truetype(MSYH, 14)
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
