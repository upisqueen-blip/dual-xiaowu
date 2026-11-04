#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 A 屏(400x300 黑白快刷)欢迎页 PNG v3：参考「居中竖排 + 粗体标题 + 黑底白字药丸」设计。

设计要点（参照用户提供的参考图）：
  - 单列居中、留白驱动层次，不使用分隔线
  - 顶部：小悟 logo（猴头水滴 + 小悟 横向组合标），纯黑白
  - 粗体欢迎标题
  - 焦点：黑底白字圆角药丸块承载售后 QQ 群（唯一的反相高亮元素）
  - 底部操作提示
  - 四角细 L 形装饰，增加产品感（墨水屏上清晰不喧宾夺主）

输出：
  firmware/s3/main/boards/custom/ssd2683za-s3-epd42/welcome.png  (烧入固件)
  design/welcome_preview.png                                     (预览)
  firmware/s3/main/boards/custom/ssd2683za-s3-epd42/welcome_png.h (C 数组，固件 include)
"""
import os
from PIL import Image, ImageDraw, ImageFont

W, H = 400, 300
FONT_DIR = "/c/Windows/Fonts"
SIMHEI = os.path.join(FONT_DIR, "simhei.ttf")      # 黑体，粗犷有力，做标题/药丸
MSYH   = os.path.join(FONT_DIR, "msyh.ttc")         # 雅黑，做提示
LOGO_SRC = r"E:\electron\screen\inksight-main\inksight-main\小悟logo.png"
OUT_FW   = r"E:\electron\screen\inksight-main\inksight-main\xiaowu-s3-open-platform\firmware\s3\main\boards\custom\ssd2683za-s3-epd42\welcome.png"
OUT_PRE  = r"E:\electron\screen\inksight-main\inksight-main\xiaowu-s3-open-platform\design\welcome_preview.png"
OUT_H    = r"E:\electron\screen\inksight-main\inksight-main\xiaowu-s3-open-platform\firmware\s3\main\boards\custom\ssd2683za-s3-epd42\welcome_png.h"

BLACK, WHITE = 0, 255
THRESH = 160  # 阈值；保留浅灰笔画（悟字细节/耳朵渐变），避免硬阈值把细笔画吃掉


def load_logo(target_w=190):
    """读取小悟 logo（白底、黑+金），扫描墨色像素取紧致包围盒（避开边缘噪点），
    阈值化为纯黑白，按目标宽度等比缩放，返回 'L' 0/255 图。"""
    im = Image.open(LOGO_SRC).convert("L")
    px = im.load()
    W0, H0 = im.size
    minx, miny, maxx, maxy = W0, H0, -1, -1
    for y in range(0, H0, 2):
        for x in range(0, W0, 2):
            if px[x, y] < 128:
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y
    if maxx >= 0:
        pad = 6
        minx = max(0, minx - pad); miny = max(0, miny - pad)
        maxx = min(W0 - 1, maxx + pad); maxy = min(H0 - 1, maxy + pad)
        im = im.crop((minx, miny, maxx + 1, maxy + 1))
    im = im.point(lambda p: BLACK if p < THRESH else WHITE)
    w, h = im.size
    scale = target_w / w
    nw, nh = max(1, round(w * scale)), max(1, round(h * scale))
    im = im.resize((nw, nh), Image.LANCZOS)
    return im


def draw_corner_ticks(d, inset=10, t=2, L=16):
    """四角细 L 形装饰，增加产品感。"""
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

    # 1) 四角 L 形装饰
    draw_corner_ticks(d)

    # 2) Logo（猴头水滴 + 小悟 横向组合标）置顶居中（深色水滴在左，整体右移几像素平衡重心）
    logo = load_logo(182)
    lx = (W - logo.width) // 2 + 6
    ly = 16
    canvas.paste(logo, (lx, ly))

    # 3) 标题：欢迎加入小悟AI智研社（粗体）
    title_font = ImageFont.truetype(SIMHEI, 26)
    d.text((W // 2, 176), "欢迎加入小悟AI智研社", font=title_font,
           fill=BLACK, anchor="mm")

    # 4) 焦点：黑底白字圆角药丸承载售后 QQ 群
    pill_text = "售后技术QQ群：65558220"
    pill_font = ImageFont.truetype(SIMHEI, 15)
    tb = d.textbbox((0, 0), pill_text, font=pill_font)
    tw, th = tb[2] - tb[0], tb[3] - tb[1]
    pad_x, pad_y = 18, 9
    pill_w, pill_h = tw + pad_x * 2, th + pad_y * 2
    pill_x = (W - pill_w) // 2
    pill_y = 214
    d.rounded_rectangle([pill_x, pill_y, pill_x + pill_w, pill_y + pill_h],
                        radius=min(12, pill_h // 2), fill=BLACK)
    d.text((W // 2, pill_y + pill_h // 2), pill_text, font=pill_font,
           fill=WHITE, anchor="mm")

    # 5) 底部操作提示（锚定屏幕底部，拉开与药丸的留白）
    hint_font = ImageFont.truetype(MSYH, 12)
    d.text((W // 2, 288), "按 OK 键进入功能菜单", font=hint_font,
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
