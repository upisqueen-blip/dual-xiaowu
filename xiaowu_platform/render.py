"""
双屏模式渲染器（自包含、无 LLM / 天气依赖）
============================================

由固件在 /api/render?persona=DUAL_A / DUAL_B 时调用，以及浏览器快照 /api/preview 调用。

- DUAL_A（A 屏 / FPC1 黑白快刷 SSD1619）：渲染便签 / 作业 / 阅读 / 相册 / 场景模块，
  输出 1bpp BMP。
- DUAL_B（B 屏 / FPC2 彩色 SSD2683ZA）：渲染今日课程表 / 相册 / 场景模块，
  输出 2bpp 彩色原始帧（0=黑 1=白 2=黄 3=红，与固件 PackInkSightFourColor 一致）。

渲染逻辑全部在服务端（复用 PIL + 自带中文字体），固件侧只负责拉图 + 刷屏，
无需在 MCU 上做文本排版，规避墨水屏字号/编码痛点。

移植自 backend/core/dual_render.py，去掉了对 image_processing / json_renderer /
mode_registry 的依赖，相册走简化版照片管线，天气模式在无数据源时降级为文字看板，
保证「开箱即跑、无外部 API 也能出图」。
"""
from __future__ import annotations

import io
import logging
import re
import time
from datetime import datetime
from pathlib import Path
from typing import Any, Optional

from PIL import Image, ImageDraw, ImageFont
from . import data_api
from .settings import settings
from .store import Store

store = Store(settings.database_path)

logger = logging.getLogger(__name__)

# 自带中文字体（项目内 fonts/ 目录，随仓库分发，无需外网下载）。
_FONT_DIR = Path(__file__).resolve().parent / "fonts"
_UPLOAD_DIR = Path(__file__).resolve().parent.parent / "data" / "uploads"

_WEEKDAY_CN = ["周一", "周二", "周三", "周四", "周五", "周六", "周日"]


# ── 字体 / 打包助手 ──────────────────────────────────────────
def _load_cjk(size: int, bold: bool = False):
    """加载中文黑体（优先自带 Noto Sans SC）；逐级回落，绝不崩溃。

    顺序：Noto Sans SC（Regular/Bold）→ Noto Serif SC → 系统微软雅黑/
    Noto CJK（Linux）/ 苹方（macOS）→ Pillow 默认字体。
    """
    candidates: list[Path] = [
        _FONT_DIR / ("NotoSansSC-Bold.otf" if bold else "NotoSansSC-Regular.otf"),
        _FONT_DIR / "NotoSerifSC-Regular.ttf",
    ]
    if bold:
        candidates.append(Path("C:/Windows/Fonts/msyhbd.ttc"))
        candidates.append(Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc"))
    candidates.append(Path("C:/Windows/Fonts/msyh.ttc"))
    candidates.append(Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"))
    candidates.append(Path("/System/Library/Fonts/PingFang.ttc"))
    for path in candidates:
        try:
            if path.is_file():
                return ImageFont.truetype(str(path), size)
        except Exception:  # noqa: BLE001
            continue
    return ImageFont.load_default()


def _load_serif(size: int, bold: bool = False):
    """加载中文宋体（欢迎页/引导页用，观感更正式）；缺字体时回落黑体。"""
    candidates: list[Path] = [
        _FONT_DIR / ("NotoSerifSC-Bold.ttf" if bold else "NotoSerifSC-Regular.ttf"),
        Path("C:/Windows/Fonts/simsun.ttc"),
        Path("/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc"),
    ]
    for path in candidates:
        try:
            if path.is_file():
                return ImageFont.truetype(str(path), size)
        except Exception:  # noqa: BLE001
            continue
    return _load_cjk(size, bold=bold)


def image_to_bmp_bytes(img: Image.Image) -> bytes:
    buf = io.BytesIO()
    img.save(buf, format="BMP")
    return buf.getvalue()


def image_to_png_bytes(img: Image.Image) -> bytes:
    if img.mode == "1":
        img = img.convert("L")
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def image_to_raw_2bpp(img: Image.Image) -> bytes:
    """4 色 e-ink 2bpp 打包：0=黑 1=白 2=黄 3=红，4 像素/字节，MSB 优先。"""
    if img.mode != "P":
        img = img.convert("P")
    pixels = img.load()
    w, h = img.size
    out = bytearray(w * h // 4)
    idx = 0
    for y in range(h):
        for x in range(0, w, 4):
            packed = 0
            for bit in range(4):
                px = x + bit
                color = (pixels[px, y] & 0x03) if px < w else 0x01
                packed |= color << (6 - bit * 2)
            out[idx] = packed
            idx += 1
    return bytes(out)


def image_to_raw_4bpp(img: Image.Image) -> bytes:
    """6 色（E6）4bpp 打包：2 像素/字节，高 nibble=左像素。"""
    if img.mode != "P":
        img = img.convert("P")
    pixels = img.load()
    w, h = img.size
    out = bytearray((w + 1) // 2 * h)
    idx = 0
    for y in range(h):
        for x in range(0, w, 2):
            hi = (pixels[x, y] & 0x0F) if x < w else 0x01
            lo = (pixels[x + 1, y] & 0x0F) if x + 1 < w else 0x01
            out[idx] = (hi << 4) | lo
            idx += 1
    return bytes(out)


def _palette_p_image(w: int, h: int, colors: int = 4) -> Image.Image:
    """彩色调色板图像：索引 0=黑 1=白 2=黄(四色)/红(三色) 3=红。"""
    img = Image.new("P", (w, h), 1)  # 默认白底
    palette = []
    for i in range(256):
        if i == 0:
            palette += [0, 0, 0]
        elif i == 1:
            palette += [255, 255, 255]
        elif i == 2:
            palette += [255, 214, 0] if colors >= 4 else [230, 30, 30]
        elif i == 3:
            palette += [230, 30, 30]
        else:
            palette += [255, 255, 255]
    img.putpalette(palette)
    return img


def _new_canvas(w: int, h: int, colors: int = 2) -> Image.Image:
    """白底画布：彩色屏必须用调色板图（索引 1 = 白），不能画在 mode "1" 上。

    1bit 图打 2bpp 裸帧时会先 convert("P")，PIL 给白底的调色板索引是 255，
    与 image_to_raw_2bpp 里的 &0x03 叠加正好等于 3（红）——这就是「引导图在
    彩色屏上整屏泛红」的根因。
    """
    if colors >= 3:
        return _palette_p_image(w, h, colors)
    return Image.new("1", (w, h), 1)


# ── 通用绘制助手 ─────────────────────────────────────────────
def _text_w(draw: ImageDraw.ImageDraw, text: str, font) -> int:
    try:
        return int(draw.textlength(text, font=font))
    except Exception:  # noqa: BLE001
        return len(text) * (font.size // 2)


def _ellipsis_text(draw: ImageDraw.ImageDraw, text: str, font, max_w: int) -> str:
    if _text_w(draw, text, font) <= max_w:
        return text
    out = text
    while out and _text_w(draw, out + "…", font) > max_w:
        out = out[:-1]
    return (out + "…") if out else "…"


def _draw_list_header(draw: ImageDraw.ImageDraw, title: str, date_str: str,
                      count_str: str, w: int, scale: float,
                      font_title, font_meta, battery: Optional[int] = None, fg=0):
    title_x = int(14 * scale)
    title_y = int(10 * scale)
    draw.text((title_x, title_y), title, fill=fg, font=font_title)
    date_x = title_x + _text_w(draw, title, font_title) + int(12 * scale)
    draw.text((date_x, title_y + max(0, (font_title.size - font_meta.size) // 2)),
              date_str, fill=fg, font=font_meta)
    cy = title_y + font_title.size // 2
    right = w - int(14 * scale)
    if battery is not None:
        pct = f"{max(0, min(100, int(battery)))}%"
        _draw_battery_with_text(draw, right, cy, scale, battery, font_meta, fg)
        right -= int(74 * scale) + int(12 * scale)
    if count_str:
        draw.text((right - _text_w(draw, count_str, font_meta),
                   title_y + max(0, (font_title.size - font_meta.size) // 2)),
                  count_str, fill=fg, font=font_meta)
    draw.line([(int(10 * scale), int(40 * scale)),
               (w - int(10 * scale), int(40 * scale))], fill=fg, width=max(1, int(2 * scale)))


def _draw_battery_indicator(draw: ImageDraw.ImageDraw, x_right: int, y_center: int,
                            scale: float, level: int, fg=0):
    level = max(0, min(100, int(level)))
    bw = int(30 * scale)
    bh = int(14 * scale)
    cap = max(2, int(3 * scale))
    lw = max(1, int(2 * scale))
    x0 = x_right - bw
    y0 = y_center - bh // 2
    draw.rectangle([x0, y0, x0 + bw, y0 + bh], outline=fg, width=lw)
    draw.rectangle([x0 + bw + lw, y0 + bh // 2 - cap // 2,
                    x0 + bw + lw + cap, y0 + bh // 2 + cap - cap // 2], fill=fg)
    inner_w = bw - lw - int(4 * scale)
    fill_w = int(inner_w * level / 100)
    if fill_w > 0:
        draw.rectangle([x0 + lw + int(1 * scale), y0 + lw + int(1 * scale),
                        x0 + lw + int(1 * scale) + fill_w,
                        y0 + bh - lw - int(1 * scale)], fill=fg)


def _draw_battery_with_text(draw: ImageDraw.ImageDraw, x_right: int, y_center: int,
                            scale: float, level: int, font, fg=0):
    text = f"{max(0, min(100, int(level)))}%"
    icon_w = int(30 * scale) + max(2, int(3 * scale)) + max(1, int(2 * scale))
    _draw_battery_indicator(draw, x_right, y_center, scale, level, fg)
    tx = x_right - icon_w - int(6 * scale) - _text_w(draw, text, font)
    draw.text((tx, y_center - font.size // 2), text, fill=fg, font=font)


def _fit_rows(n: int, top: int, bottom: int, scale: float,
              overflow_reserve: int = 0) -> tuple[int, int, int]:
    avail = bottom - top
    row_h = max(int(26 * scale), min(int(38 * scale), avail // max(1, n)))
    fit = max(1, avail // row_h)
    if n > fit and overflow_reserve:
        bottom -= overflow_reserve
        avail = bottom - top
        row_h = max(int(26 * scale), min(int(38 * scale), avail // n))
        fit = max(1, avail // row_h)
    return fit, row_h, bottom


def _draw_overflow_hint(draw: ImageDraw.ImageDraw, w: int, h: int, scale: float,
                        font, hidden: int, unit: str = "条", fg=0):
    if hidden <= 0:
        return
    text = f"…还有 {hidden} {unit}"
    draw.text((w - int(14 * scale) - _text_w(draw, text, font), h - int(16 * scale)),
              text, fill=fg, font=font)


# ── 相册（极简照片管线，无外部依赖）───────────────────────────
def _album_photos(dual_config: Optional[dict]) -> list[dict]:
    content = (dual_config or {}).get("content") or {}
    album = content.get("album") or {}
    if not isinstance(album, dict):
        return []
    photos = album.get("photos") or []
    out = []
    for p in photos:
        if isinstance(p, dict) and (p.get("id") or p.get("url")):
            out.append(p)
    return out


def _load_album_image(photo: dict) -> Optional[Image.Image]:
    pid = (photo.get("id") or "").strip()
    if not pid and photo.get("url"):
        m = re.search(r"/uploads/([0-9a-fA-F]{24}|\S+)", str(photo.get("url")))
        if m:
            pid = m.group(1)
    if not pid:
        return None
    path = _UPLOAD_DIR / f"{pid}.bin"
    if not path.is_file():
        # 也兼容直接存原扩展名
        for ext in (".png", ".jpg", ".jpeg", ".webp"):
            alt = _UPLOAD_DIR / f"{pid}{ext}"
            if alt.is_file():
                path = alt
                break
    if not path.is_file():
        return None
    try:
        with Image.open(path) as img:
            return img.convert("RGB")
    except Exception as exc:  # noqa: BLE001
        logger.warning("[DUAL-RENDER] album image load failed id=%s: %s", pid, exc)
        return None


def _fit_cover(src: Image.Image, w: int, h: int) -> Image.Image:
    """居中裁切铺满 (w×h)，保持比例。"""
    sw, sh = src.size
    scale = max(w / sw, h / sh)
    tw, th = int(sw * scale), int(sh * scale)
    resized = src.resize((tw, th), Image.LANCZOS)
    left = (tw - w) // 2
    top = (th - h) // 2
    return resized.crop((left, top, left + w, top + h))


def _quantize_color(src: Image.Image, colors: int) -> Image.Image:
    """把照片量化到面板 4 色调色板（索引 0=黑 1=白 2=黄/红 3=红），配 Floyd 抖动。"""
    pal_img = _palette_p_image(src.width, src.height, colors)
    return src.convert("RGB").quantize(palette=pal_img, dither=Image.FLOYDSTEINBERG)


def _render_album_empty(w: int, h: int, colors: int, msg: str) -> Image.Image:
    scale = w / 400.0
    if colors >= 3:
        img = _palette_p_image(w, h, colors)
    else:
        img = Image.new("1", (w, h), 1)
    draw = ImageDraw.Draw(img)
    draw.text((int(14 * scale), int(70 * scale)), msg, fill=0, font=_load_cjk(max(14, int(18 * scale))))
    return img


def render_album(mac: str, w: int, h: int, colors: int,
                 dual_config: Optional[dict]) -> Image.Image:
    photos = _album_photos(dual_config)
    if not photos:
        return _render_album_empty(w, h, colors, "暂无照片")
    today = datetime.now()
    base_idx = (today.month * 31 + today.day) % len(photos)
    src = None
    photo = None
    for step in range(len(photos)):
        candidate = photos[(base_idx + step) % len(photos)]
        loaded = _load_album_image(candidate)
        if loaded is not None:
            src = loaded
            photo = candidate
            break
    if src is None:
        return _render_album_empty(w, h, colors, "照片读取失败")

    scale = w / 400.0
    caption = (photo.get("caption") or "").strip()
    cap_h = int(26 * scale) if caption else 0
    box_h = max(1, h - cap_h)
    fitted = _fit_cover(src, w, box_h)
    canvas = Image.new("RGB", (w, h), (255, 255, 255))
    canvas.paste(fitted, (0, 0))

    if colors >= 3:
        img = _quantize_color(canvas, colors)
    else:
        img = canvas.convert("L").convert("1")  # PIL 默认 Floyd 抖动

    if caption:
        draw = ImageDraw.Draw(img)
        by = h - int(24 * scale)
        draw.rectangle([0, by - int(4 * scale), w, h], fill=1)
        font = _load_cjk(max(12, int(15 * scale)))
        draw.text((int(14 * scale), by), _ellipsis_text(draw, caption, font, w - int(28 * scale)),
                  fill=0, font=font)
    return img


# ── 便签底色 ─────────────────────────────────────────────────
_NOTE_RGB = {
    "yellow": (255, 243, 199), "pink": (253, 228, 236), "blue": (220, 235, 251),
    "green": (223, 242, 228), "white": (255, 255, 255),
}


def _a_mode_has_data(mode: str, dual_config: Optional[dict]) -> bool:
    content = (dual_config or {}).get("content") or {}
    today = datetime.now().strftime("%Y-%m-%d")
    if mode == "homework":
        items = content.get("homework") or []
        return any(isinstance(it, dict) and (it.get("date") or today) == today for it in items)
    if mode == "reading":
        return bool(content.get("reading"))
    if mode == "notes":
        items = content.get("notes") or []
        return any(isinstance(n, dict) and (n.get("persistent") or (n.get("date") or today) == today) for n in items)
    if mode == "album":
        return bool(_album_photos(dual_config))
    return True


# ── A 屏：便签 / 作业 / 阅读 ─────────────────────────────────
def render_notes(mac: str, w: int, h: int, dual_config: Optional[dict],
                 battery: Optional[int] = None) -> Image.Image:
    scale = w / 400.0
    img = Image.new("1", (w, h), 1)
    draw = ImageDraw.Draw(img)
    font_title = _load_cjk(max(16, int(22 * scale)))
    font_body = _load_cjk(max(13, int(16 * scale)))
    font_meta = _load_cjk(max(11, int(13 * scale)))

    today = datetime.now()
    today_str = today.strftime("%Y-%m-%d")
    content = (dual_config or {}).get("content") or {}
    notes = content.get("notes") or []
    cleaned: list[tuple[str, str, str]] = []
    for n in notes:
        if isinstance(n, dict):
            text = (n.get("text") or "").strip()
            if not text:
                continue
            date = n.get("date") or today_str
            if not (bool(n.get("persistent")) or date == today_str):
                continue
            sender = n.get("sender") or "家长"
            st = n.get("sender_type") or ("teacher" if sender == "老师" else "")
        elif isinstance(n, str) and n.strip():
            text = n.strip()
            sender = "家长"
            st = "parent"
        else:
            continue
        cleaned.append((text, sender, st))

    _draw_list_header(draw, "老师便签", f"{today.month}月{today.day}日",
                      f"共 {len(cleaned)} 条", w, scale, font_title, font_meta, battery)
    if not cleaned:
        draw.text((int(14 * scale), int(70 * scale)), "暂无通知", fill=0, font=font_body)
        return img

    top = int(52 * scale)
    bottom = int(h - 12 * scale)
    fit, row_h, _ = _fit_rows(len(cleaned), top, bottom, scale, int(28 * scale))
    y = top
    max_w = w - int(14 * scale) * 2 - int(24 * scale)
    for i, (text, sender, st) in enumerate(cleaned[:fit]):
        cy = y + max(2, (row_h - font_body.size) // 2)
        draw.rectangle([int(14 * scale), y + int(7 * scale),
                        int(14 * scale) + max(2, int(4 * scale)),
                        y + row_h - int(7 * scale)], fill=0)
        prefix = "师·" if st == "teacher" else ""
        full = (prefix + text) if prefix else text
        draw.text((int(28 * scale), cy), _ellipsis_text(draw, full, font_body, max_w),
                  fill=0, font=font_body)
        y += row_h
        if i < fit - 1:
            draw.line([(int(10 * scale), y), (w - int(10 * scale), y)], fill=0, width=1)
    _draw_overflow_hint(draw, w, h, scale, font_meta, len(cleaned) - fit)
    return img


def _collect_items(raw, text_key: str) -> list[dict]:
    out: list[dict] = []
    for it in raw or []:
        if isinstance(it, dict):
            text = (it.get(text_key) or "").strip()
            if not text:
                continue
            item = dict(it)
            item[text_key] = text
            out.append(item)
        elif isinstance(it, str) and it.strip():
            out.append({text_key: it.strip()})
    return out


def render_homework(mac: str, w: int, h: int, dual_config: Optional[dict],
                    battery: Optional[int] = None) -> Image.Image:
    scale = w / 400.0
    img = Image.new("1", (w, h), 1)
    draw = ImageDraw.Draw(img)
    font_title = _load_cjk(max(16, int(22 * scale)))
    font_body = _load_cjk(max(13, int(16 * scale)))
    font_meta = _load_cjk(max(11, int(13 * scale)))

    today = datetime.now()
    today_str = today.strftime("%Y-%m-%d")
    content = (dual_config or {}).get("content") or {}
    items = _collect_items(content.get("homework"), "text")
    cleaned: list[tuple[str, bool, str, str]] = []
    for it in items:
        if (it.get("date") or today_str) != today_str:
            continue
        sender = it.get("sender") or "家长"
        st = it.get("sender_type") or ("teacher" if sender == "老师" else "")
        cleaned.append((it["text"], bool(it.get("done")), sender, st))

    done_n = sum(1 for _, d, _, _ in cleaned if d)
    _draw_list_header(draw, "今日作业", f"{today.month}月{today.day}日",
                      f"共 {len(cleaned)} 项", w, scale, font_title, font_meta, battery)
    if not cleaned:
        draw.text((int(14 * scale), int(70 * scale)), "暂无作业", fill=0, font=font_body)
        return img

    top = int(52 * scale)
    bottom = int(h - 12 * scale)
    fit, row_h, _ = _fit_rows(len(cleaned), top, bottom, scale, int(28 * scale))
    y = top
    box = max(8, int(13 * scale))
    max_w = w - int(14 * scale) * 2 - box - int(12 * scale)
    for i, (text, done, sender, st) in enumerate(cleaned[:fit]):
        cy = y + max(2, (row_h - font_body.size) // 2)
        by = y + (row_h - box) // 2
        draw.rectangle([int(14 * scale), by, int(14 * scale) + box, by + box],
                       outline=0, width=max(1, int(2 * scale)))
        if done:
            draw.rectangle([int(14 * scale) + max(1, int(2 * scale)),
                            by + max(1, int(2 * scale)),
                            int(14 * scale) + box - max(1, int(2 * scale)),
                            by + box - max(1, int(2 * scale))], fill=0)
        prefix = "师·" if st == "teacher" else ""
        full = (prefix + text) if prefix else text
        shown = _ellipsis_text(draw, full, font_body, max_w)
        draw.text((int(14 * scale) + box + int(12 * scale), cy), shown, fill=0, font=font_body)
        if done:
            tx = int(14 * scale) + box + int(12 * scale)
            ty = cy + font_body.size // 2
            draw.line([(tx, ty), (tx + _text_w(draw, shown, font_body), ty)],
                      fill=0, width=max(1, int(2 * scale)))
        y += row_h
        if i < fit - 1:
            draw.line([(int(10 * scale), y), (w - int(10 * scale), y)], fill=0, width=1)
    _draw_overflow_hint(draw, w, h, scale, font_meta, len(cleaned) - fit, "项")
    if len(cleaned) <= fit:
        draw.text((int(14 * scale), h - int(16 * scale)),
                  f"已完成 {done_n}/{len(cleaned)}", fill=0, font=font_meta)
    return img


def render_reading(mac: str, w: int, h: int, dual_config: Optional[dict],
                   battery: Optional[int] = None) -> Image.Image:
    scale = w / 400.0
    img = Image.new("1", (w, h), 1)
    draw = ImageDraw.Draw(img)
    font_title = _load_cjk(max(16, int(22 * scale)))
    font_body = _load_cjk(max(13, int(16 * scale)))
    font_meta = _load_cjk(max(11, int(13 * scale)))

    today = datetime.now()
    content = (dual_config or {}).get("content") or {}
    items = _collect_items(content.get("reading"), "title")
    cleaned = [(it["title"], (it.get("progress") or "").strip()) for it in items]

    _draw_list_header(draw, "阅读任务", f"{today.month}月{today.day}日",
                      f"共 {len(cleaned)} 项", w, scale, font_title, font_meta, battery)
    if not cleaned:
        draw.text((int(14 * scale), int(70 * scale)), "暂无阅读任务", fill=0, font=font_body)
        return img

    top = int(52 * scale)
    bottom = int(h - 12 * scale)
    fit, row_h, _ = _fit_rows(len(cleaned), top, bottom, scale, int(28 * scale))
    y = top
    for i, (title, progress) in enumerate(cleaned[:fit]):
        cy = y + max(2, (row_h - font_body.size) // 2)
        shown_title = title if title.startswith(("《", "「", "\"")) else f"《{title}》"
        meta_w = _text_w(draw, progress, font_meta) + int(16 * scale) if progress else 0
        max_w = w - int(14 * scale) * 2 - meta_w - int(14 * scale)
        draw.text((int(14 * scale), cy), _ellipsis_text(draw, shown_title, font_body, max_w),
                  fill=0, font=font_body)
        if progress:
            draw.text((w - int(14 * scale) - _text_w(draw, progress, font_meta), cy),
                      progress, fill=0, font=font_meta)
        y += row_h
        if i < fit - 1:
            draw.line([(int(10 * scale), y), (w - int(10 * scale), y)], fill=0, width=1)
    _draw_overflow_hint(draw, w, h, scale, font_meta, len(cleaned) - fit, "项")
    return img


# ── B 屏：课程表 ─────────────────────────────────────────────
def _collect_today_courses(dual_config: Optional[dict], today: datetime) -> tuple[list[dict], int]:
    content = (dual_config or {}).get("content") or {}
    ct = content.get("courseTable") or {}
    periods = ct.get("periods") or []
    grid = ct.get("grid") or []
    today_idx = today.weekday()
    rows: list[dict] = []
    now_min = today.hour * 60 + today.minute
    current_idx = -1
    for p, per in enumerate(periods):
        cell = None
        if isinstance(grid, list) and today_idx < len(grid) and p < len(grid[today_idx]):
            cell = grid[today_idx][p]
        if isinstance(cell, dict) and cell.get("name"):
            sh = int(per.get("start_hour", 0))
            sm = int(per.get("start_minute", 0))
            eh = int(per.get("end_hour", sh))
            em = int(per.get("end_minute", sm + 40))
            start = sh * 60 + sm
            end = eh * 60 + em
            rows.append({
                "idx": p, "time": f"{sh:02d}:{sm:02d}", "start": start, "end": end,
                "name": cell.get("name", ""), "teacher": cell.get("teacher") or "",
                "room": cell.get("room") or "",
            })
            if start <= now_min < end and current_idx < 0:
                current_idx = len(rows) - 1
    if current_idx < 0:
        for i, r in enumerate(rows):
            if r["start"] > now_min:
                current_idx = i
                break
    return rows, current_idx


def render_course(mac: str, w: int, h: int, colors: int,
                  dual_config: Optional[dict]) -> Image.Image:
    scale = w / 400.0
    is_color = colors >= 3
    img = _palette_p_image(w, h, colors) if is_color else Image.new("1", (w, h), 1)
    draw = ImageDraw.Draw(img)
    fg = 0

    font_title = _load_cjk(max(16, int(24 * scale)))
    font_body = _load_cjk(max(12, int(16 * scale)))
    font_meta = _load_cjk(max(10, int(12 * scale)))
    font_period = _load_cjk(max(11, int(14 * scale)))

    content = (dual_config or {}).get("content") or {}
    ct = content.get("courseTable") or {}
    enabled = ct.get("enabled", True)
    school = (ct.get("school") or "").strip()
    class_name = (ct.get("class_name") or "").strip()

    today = datetime.now()
    today_idx = today.weekday()
    date_str = f"{today.month}月{today.day}日 {_WEEKDAY_CN[today_idx]}"
    title = class_name or school or "课程表"

    draw.text((int(14 * scale), int(10 * scale)), title, fill=fg, font=font_title)
    draw.text((w - int(14 * scale) - _text_w(draw, date_str, font_meta), int(16 * scale)),
              date_str, fill=fg, font=font_meta)
    draw.line([(int(10 * scale), int(44 * scale)),
               (w - int(10 * scale), int(44 * scale))], fill=fg, width=max(1, int(2 * scale)))

    if not enabled:
        draw.text((int(14 * scale), int(70 * scale)), "课程表未启用", fill=fg, font=font_body)
        return img

    rows, current_idx = _collect_today_courses(dual_config, today)
    top = int(54 * scale)
    bottom = int(h - 12 * scale)
    if not rows:
        draw.text((int(14 * scale), top + int(10 * scale)), "今日无课", fill=fg, font=font_body)
        return img

    avail_h = bottom - top
    row_h = max(int(26 * scale), min(int(40 * scale), avail_h // len(rows)))
    fit = max(1, avail_h // row_h)
    if len(rows) > fit:
        bottom = int(h - 26 * scale)
        avail_h = bottom - top
        row_h = max(int(26 * scale), min(int(40 * scale), avail_h // len(rows)))
        fit = max(1, avail_h // row_h)
    y = top
    for i, r in enumerate(rows[:fit]):
        hl = is_color and i == current_idx
        if hl:
            draw.rectangle([int(10 * scale), y, w - int(10 * scale), y + row_h - 2], fill=2)
        col = 3 if (hl and colors >= 4) else (1 if hl else fg)
        cy = y + max(2, (row_h - font_body.size) // 2)
        draw.text((int(14 * scale), cy), f"{r['idx'] + 1}", fill=col, font=font_period)
        draw.text((int(42 * scale), cy), r["time"], fill=col, font=font_meta)
        draw.text((int(104 * scale), cy), r["name"], fill=col, font=font_body)
        meta = ""
        if r["teacher"]:
            meta += f"{r['teacher']}"
        if r["room"]:
            meta += f" · {r['room']}" if r["teacher"] else f"{r['room']}"
        if meta:
            draw.text((w - int(14 * scale) - _text_w(draw, meta, font_meta), cy),
                      meta, fill=col, font=font_meta)
        y += row_h
        draw.line([(int(10 * scale), y), (w - int(10 * scale), y)], fill=fg, width=1)
    if len(rows) > fit:
        draw.text((w - int(14 * scale) - _text_w(draw, f"…还有 {len(rows) - fit} 节", font_meta),
                   h - int(14 * scale)), f"…还有 {len(rows) - fit} 节", fill=fg, font=font_meta)
    return img


def render_scene_modules(title: str, modules: list, w: int, h: int, colors: int,
                         footer: str = "") -> Image.Image:
    scale = w / 400.0
    img = _palette_p_image(w, h, colors) if colors >= 3 else Image.new("1", (w, h), 1)
    draw = ImageDraw.Draw(img)
    fg = 0
    font_title = _load_cjk(max(16, int(22 * scale)))
    font_body = _load_cjk(max(13, int(16 * scale)))
    font_meta = _load_cjk(max(10, int(12 * scale)))
    draw.text((int(14 * scale), int(10 * scale)), title[:24], fill=fg, font=font_title)
    draw.text((w - int(14 * scale) - _text_w(draw, datetime.now().strftime("%m月%d日"), font_meta), int(16 * scale)),
              datetime.now().strftime("%m月%d日"), fill=fg, font=font_meta)
    draw.line([(int(10 * scale), int(40 * scale)), (w - int(10 * scale), int(40 * scale))], fill=fg, width=max(1, int(2 * scale)))
    rows = [str(item).strip() for item in modules if str(item).strip()][:6]
    top = int(52 * scale)
    row_h = max(int(26 * scale), min(int(38 * scale), (h - top - int(26 * scale)) // max(1, len(rows))))
    for index, row in enumerate(rows):
        y = top + index * row_h
        draw.text((int(14 * scale), y + max(1, (row_h - font_body.size) // 2)), row[:28], fill=fg, font=font_body)
        if index < len(rows) - 1:
            draw.line([(int(10 * scale), y + row_h), (w - int(10 * scale), y + row_h)], fill=fg, width=1)
    if footer:
        draw.text((int(14 * scale), h - int(18 * scale)), footer[:36], fill=fg, font=font_meta)
    return img


def _weather_kind(desc, code) -> str:
    """把天气文案/编码映射成线稿图标类型：sun/cloud/rain/partly。"""
    d = str(desc)
    try:
        c = int(code)
    except (TypeError, ValueError):
        c = -1
    if "雨" in d or c >= 50:
        return "rain"
    if "晴" in d and "云" in d:
        return "partly"
    if "晴" in d:
        return "sun"
    return "cloud"


def _draw_weather_icon(draw, kind, x, y, size, fg=0):
    """三色墨水屏适用的极简线稿天气图标（黑色描边，无填充，避免灰阶）。"""
    import math
    lw = max(1, int(size / 20))
    if kind in ("cloud", "rain", "partly"):
        cy = y + size * (0.64 if kind == "partly" else 0.56)
        draw.line([(x + size * 0.12, cy), (x + size * 0.88, cy)], fill=fg, width=lw)
        draw.ellipse([x + size * 0.08, cy - size * 0.30, x + size * 0.40, cy + size * 0.02], outline=fg, width=lw)
        draw.ellipse([x + size * 0.32, cy - size * 0.42, x + size * 0.74, cy - size * 0.04], outline=fg, width=lw)
        draw.ellipse([x + size * 0.56, cy - size * 0.28, x + size * 0.92, cy + size * 0.04], outline=fg, width=lw)
    if kind in ("sun", "partly"):
        sx = x + size * 0.30 if kind == "partly" else x + size * 0.5
        sy = y + size * 0.26 if kind == "partly" else y + size * 0.5
        r = size * (0.13 if kind == "partly" else 0.20)
        draw.ellipse([sx - r, sy - r, sx + r, sy + r], outline=fg, width=lw)
        for deg in range(0, 360, 45):
            a = math.radians(deg)
            x1 = sx + (r + size * 0.05) * math.cos(a)
            y1 = sy + (r + size * 0.05) * math.sin(a)
            x2 = sx + (r + size * 0.18) * math.cos(a)
            y2 = sy + (r + size * 0.18) * math.sin(a)
            draw.line([(x1, y1), (x2, y2)], fill=fg, width=lw)
    if kind == "rain":
        cy = y + size * 0.56
        for i in range(3):
            rx0 = x + size * (0.30 + i * 0.18)
            draw.line([(rx0, cy + size * 0.08), (rx0 - size * 0.06, cy + size * 0.28)], fill=fg, width=lw)


def render_weather(weather: dict, w: int, h: int, colors: int) -> Image.Image:
    """渲染成都天气卡片（B 屏默认，三色墨水屏：黑/白/黄强调色）。

    布局对齐后台设计稿：顶部城市 + 「天气」+ 节气黄标，中部左温区右日期
    （竖线分隔），三日预报，底部温馨提示与日出日落，页脚小悟AI·实时。
    数据来自 console_api._weather_payload（演示数据，替换该函数即可接真实源）。
    """
    s = w / 400.0
    is_color = colors >= 3
    img = _palette_p_image(w, h, colors) if is_color else Image.new("1", (w, h), 1)
    draw = ImageDraw.Draw(img)
    fg = 0          # 黑
    wht = 1         # 白
    acc = 2 if is_color else 0   # 三色第3色：colors>=4 黄，==3 红
    pad = int(14 * s)

    f_city = _load_cjk(max(20, int(26 * s)))
    f_pin = _load_cjk(max(9, int(11 * s)))
    f_temp = _load_cjk(max(34, int(48 * s)))
    f_desc = _load_cjk(max(13, int(17 * s)))
    f_meta = _load_cjk(max(10, int(12 * s)))
    f_big = _load_cjk(max(28, int(34 * s)))
    f_sm = _load_cjk(max(9, int(11 * s)))
    f_fc = _load_cjk(max(9, int(11 * s)))

    now = datetime.now()
    city = str(weather.get("city") or "成都")
    pinyin = str(weather.get("city_pinyin") or "")
    solar = "{} · 第{}天".format(weather.get("solar_term") or "—", weather.get("solar_term_day") or "1")
    cur = str(weather.get("current_temp") or "--")
    desc = str(weather.get("today_desc") or "晴")
    feels = str(weather.get("feels_like") or "--")
    low = str(weather.get("today_low") or "--")
    high = str(weather.get("today_high") or "--")
    humidity = str(weather.get("today_humidity") or "")
    wind = f"{weather.get('today_wind_dir') or ''} {weather.get('today_wind_level') or ''}".strip()
    plate = str(weather.get("plate_limit") or "不限行")
    advice = str(weather.get("advice") or "")
    sunrise = str(weather.get("sunrise") or "--:--")
    sunset = str(weather.get("sunset") or "--:--")
    lunar_date = str(weather.get("lunar_date") or "")
    lunar_year = str(weather.get("lunar_year") or "")
    zodiac = str(weather.get("lunar_zodiac") or "")
    weekday_cn = ["周一", "周二", "周三", "周四", "周五", "周六", "周日"][now.weekday()]
    week_of_month = (now.day - 1) // 7 + 1
    day_num = str(now.day)
    date_label = f"{now.year}年{now.month}月"
    updated = str(weather.get("updated_at") or now.strftime("%Y-%m-%d %H:%M"))

    # ---- 顶部：城市 + 「天气」+ 节气黄标 ----
    draw.text((pad, int(8 * s)), city, fill=fg, font=f_city)
    if pinyin:
        draw.text((pad, int(8 * s) + f_city.size), pinyin, fill=fg, font=f_pin)
    st_w = _text_w(draw, solar, f_meta) + int(16 * s)
    st_x = w - pad - st_w
    draw.text((st_x - _text_w(draw, "天气", f_desc) - int(8 * s), int(10 * s)), "天气", fill=fg, font=f_desc)
    draw.rectangle([st_x, int(10 * s), st_x + st_w, int(10 * s) + int(20 * s)], fill=acc)
    draw.text((st_x + int(8 * s), int(13 * s)), solar, fill=fg, font=f_meta)
    draw.line([(pad, int(46 * s)), (w - pad, int(46 * s))], fill=fg, width=max(1, int(2 * s)))

    # ---- 中部：左温区 / 右日期（竖线分隔）----
    y_top = int(54 * s)
    vx = int(200 * s)
    draw.line([(vx, y_top), (vx, int(186 * s))], fill=fg, width=max(1, int(1.5 * s)))

    # 左：大温度 + 描述 + 体感/湿度/风力 + 雨云图标
    draw.text((pad, y_top), cur, fill=fg, font=f_temp)
    tw = _text_w(draw, cur, f_temp)
    draw.text((pad + tw + int(14 * s), y_top + int(8 * s)), desc, fill=fg, font=f_desc)
    _draw_weather_icon(draw, _weather_kind(desc, weather.get("today_code")),
                       vx - int(54 * s), y_top, int(50 * s), fg)
    meta_left = []
    if feels:
        meta_left.append(f"体感 {feels}")
    if humidity:
        meta_left.append(f"湿度 {humidity}")
    if wind:
        meta_left.append(f"风力 {wind}")
    for i, m in enumerate(meta_left):
        draw.text((pad, y_top + int(54 * s) + i * int(16 * s)), m, fill=fg, font=f_meta)

    # 右：日期块
    rx = vx + int(14 * s)
    draw.text((rx, y_top), f"{date_label} · 第{week_of_month}周", fill=fg, font=f_meta)
    draw.text((rx, y_top + int(18 * s)), day_num, fill=fg, font=f_big)
    rw = _text_w(draw, day_num, f_big)
    draw.text((rx + rw + int(8 * s), y_top + int(34 * s)), weekday_cn, fill=fg, font=f_meta)
    yy = y_top + int(56 * s)
    if lunar_date:
        draw.text((rx, yy), lunar_date, fill=fg, font=f_meta)
    if lunar_year or zodiac:
        draw.text((rx, yy + int(16 * s)), f"{lunar_year} / {zodiac}", fill=fg, font=f_sm)
    sy = yy + int(34 * s)
    sl_w = _text_w(draw, solar, f_sm) + int(14 * s)
    draw.rectangle([rx, sy, rx + sl_w, sy + int(16 * s)], fill=acc)
    draw.text((rx + int(7 * s), sy + int(1 * s)), solar, fill=fg, font=f_sm)
    by = sy + int(24 * s)
    plate_txt = f"限行 {plate}"
    pb_w = _text_w(draw, plate_txt, f_sm) + int(14 * s)
    draw.rectangle([rx, by, rx + pb_w, by + int(16 * s)], fill=fg)
    draw.text((rx + int(7 * s), by + int(1 * s)), plate_txt, fill=wht, font=f_sm)

    # ---- 三日预报 ----
    y_fc = int(196 * s)
    draw.text((pad, y_fc), "三日天气 · 最高/最低/℃", fill=fg, font=f_meta)
    y_cells = y_fc + int(16 * s)
    cell_h = int(44 * s)
    inner_w = w - 2 * pad
    n = 3
    gap = int(6 * s)
    cw = (inner_w - gap * (n - 1)) // n
    fc = weather.get("forecast") or []
    icon_size = int(22 * s)
    for i in range(n):
        cx = pad + i * (cw + gap)
        draw.rectangle([cx, y_cells, cx + cw, y_cells + cell_h], outline=fg, width=max(1, int(1.5 * s)))
        item = fc[i] if i < len(fc) else {}
        d = str(item.get("day") or "")
        dt = str(item.get("date") or "")
        rng = str(item.get("temp_range") or "")
        dd = str(item.get("desc") or "")
        draw.text((cx + int(6 * s), y_cells + int(3 * s)), f"{d} {dt}", fill=fg, font=f_fc)
        draw.text((cx + int(6 * s), y_cells + int(20 * s)), dd, fill=fg, font=f_fc)
        draw.text((cx + int(6 * s), y_cells + int(33 * s)), rng, fill=fg, font=f_fc)
        # 图标固定在单元格右上角，左侧三行文本独立成列，避免与温度文字碰触
        _draw_weather_icon(draw, _weather_kind(dd, item.get("code")),
                           cx + cw - icon_size - int(4 * s), y_cells + int(4 * s), icon_size, fg)

    # ---- 底部：温馨提示 + 日出日落 ----
    y_bot = y_cells + cell_h + int(8 * s)
    tip_core = advice if advice else "今日天气平和"
    tb_w = _text_w(draw, "温馨提示", f_sm) + int(12 * s)
    draw.rectangle([pad, y_bot, pad + tb_w, y_bot + int(18 * s)], fill=fg)
    draw.text((pad + int(6 * s), y_bot + int(2 * s)), "温馨提示", fill=wht, font=f_sm)
    # 先确定右侧「日出/日落」位置，再限制左侧提示文案宽度，避免两行相撞/裁切
    sr = f"日出 {sunrise}  日落 {sunset}"
    sr_w = _text_w(draw, sr, f_sm)
    sr_x = w - pad - sr_w
    tip_x = pad + tb_w + int(8 * s)
    max_tip_w = max(int(20 * s), sr_x - tip_x - int(8 * s))
    draw.text((tip_x, y_bot + int(2 * s)),
              _ellipsis_text(draw, tip_core, f_sm, max_tip_w), fill=fg, font=f_sm)
    draw.text((sr_x, y_bot + int(2 * s)), sr, fill=fg, font=f_sm)

    # ---- 页脚 ----
    y_foot = y_bot + int(24 * s)
    draw.line([(pad, y_foot - int(4 * s)), (w - pad, y_foot - int(4 * s))], fill=fg, width=1)
    draw.text((pad, y_foot), f"天气预报 · {updated}", fill=fg, font=f_pin)
    draw.text((w - pad - _text_w(draw, "小悟AI · 实时", f_pin), y_foot), "小悟AI · 实时", fill=fg, font=f_pin)
    return img


def render_family_express_bills(express: list[dict], bills: list[dict], w: int, h: int,
                                colors: int) -> Image.Image:
    scale = w / 400.0
    img = _palette_p_image(w, h, colors) if colors >= 3 else Image.new("1", (w, h), 1)
    draw = ImageDraw.Draw(img)
    fg = 0
    title_font = _load_cjk(max(16, int(22 * scale)))
    head_font = _load_cjk(max(13, int(16 * scale)))
    body_font = _load_cjk(max(10, int(13 * scale)))
    meta_font = _load_cjk(max(9, int(11 * scale)))
    pad = int(14 * scale)
    mid = w // 2
    draw.text((pad, int(10 * scale)), "家庭快递与缴费", fill=fg, font=title_font)
    draw.text((w - pad - _text_w(draw, datetime.now().strftime("%m月%d日"), meta_font), int(16 * scale)),
              datetime.now().strftime("%m月%d日"), fill=fg, font=meta_font)
    draw.line([(pad, int(42 * scale)), (w - pad, int(42 * scale))], fill=fg, width=max(1, int(2 * scale)))
    gutter = int(10 * scale)
    col_w = (w - 2 * pad - gutter) // 2
    x_left, x_right = pad, pad + col_w + gutter
    top = int(52 * scale)
    draw.text((x_left, top), f"快递/取件码 {len(express)}", fill=fg, font=head_font)
    draw.text((x_right, top), f"缴费台账 {len(bills)}", fill=fg, font=head_font)
    draw.line([(x_left, top + int(24 * scale)), (x_left + col_w, top + int(24 * scale))], fill=fg, width=1)
    draw.line([(x_right, top + int(24 * scale)), (x_right + col_w, top + int(24 * scale))], fill=fg, width=1)
    row_top = top + int(32 * scale)
    row_h = max(int(28 * scale), (h - row_top - int(26 * scale)) // 4)
    left_rows = [f"{x.get('pickupCode') or x.get('trackingNo') or '待补取件码'} {x.get('pickupPoint') or x.get('item') or ''}".strip() for x in express[:4]] or ["暂无待取包裹"]
    right_rows = [f"{x.get('name') or x.get('category') or '账单'} ¥{x.get('amount', 0)}" for x in bills[:4]] or ["暂无待缴项目"]
    max_chars = max(5, int(col_w / max(1, body_font.size * 0.95)))
    for idx in range(4):
        y = row_top + idx * row_h
        for x, rows in ((x_left, left_rows), (x_right, right_rows)):
            if idx < len(rows):
                text = str(rows[idx])
                if len(text) > max_chars:
                    text = text[:max(1, max_chars - 1)] + "…"
                draw.text((x, y), text, fill=fg, font=body_font)
            if idx < 3:
                draw.line([(x, y + row_h - int(5 * scale)), (x + col_w, y + row_h - int(5 * scale))], fill=fg, width=1)
    draw.line([(mid, top), (mid, h - int(22 * scale))], fill=fg, width=1)
    draw.text((pad, h - int(18 * scale)), "取件码手动登记 · 待缴账单提醒", fill=fg, font=meta_font)
    return img


def _draw_message_badge(img: Image.Image, unread: int, colors: int = 2) -> Image.Image:
    """在设备画面左下角叠加「未读留言」徽标。

    黑白屏：黑底白字；三色/四色屏：红底白字（更醒目）。
    白色外描边保证它叠在便签/课程表内容上依然清晰可辨。
    固件侧看到徽标即提示有留言，播放后（played_at 有值）徽标自动消失。
    """
    if unread <= 0:
        return img
    w, h = img.size
    scale = w / 400.0
    draw = ImageDraw.Draw(img)
    is_palette = img.mode == "P"
    bg = 3 if (is_palette and colors >= 3) else 0  # 索引 3=红，0=黑
    plate = 1  # 索引 1=白（mode "1" 下 1 也是白）
    text = f"留言 {unread}" if unread < 100 else "留言 99+"
    font = _load_cjk(max(11, int(13 * scale)))
    pad_x = max(4, int(7 * scale))
    bh = font.size + max(6, int(8 * scale))
    x0 = int(10 * scale)
    y0 = h - bh - int(8 * scale)
    x1 = x0 + _text_w(draw, text, font) + pad_x * 2
    y1 = y0 + bh
    outline = max(1, int(2 * scale))
    draw.rounded_rectangle([x0 - outline, y0 - outline, x1 + outline, y1 + outline],
                           radius=max(2, int(4 * scale)), fill=plate)
    draw.rounded_rectangle([x0, y0, x1, y1], radius=max(2, int(3 * scale)), fill=bg)
    draw.text((x0 + pad_x, y0 + max(2, int(4 * scale))), text, fill=plate, font=font)
    return img


def _render_dual_screen_inner(mac: str, screen: str, w: int, h: int, colors: int,
                              dual_config: Optional[dict],
                              battery: Optional[int] = None) -> Image.Image:
    """统一入口：screen='A' 渲染便签/作业/阅读/相册/场景，screen='B' 渲染课程表/相册/场景。"""
    try:
        if screen == "A":
            cfg_root = dual_config or {}
            scene_key = str(cfg_root.get("sceneKey") or "study")
            scene_cfg = (cfg_root.get("config") or {}).get(scene_key) or {}
            a_cfg = scene_cfg.get("a") or {}
            mode = str(a_cfg.get("mode") or "notes")
            if mode == "published-app":
                return _render_published_app(a_cfg, w, h, colors, screen="A")
            photos = _album_photos(dual_config)
            if scene_key == "family" and mode.startswith("family"):
                content = cfg_root.get("content") or {}
                modules = a_cfg.get("modules") or []
                express = [x for x in content.get("express", []) if isinstance(x, dict) and x.get("status") not in ("signed", "exception")]
                bills = [x for x in content.get("bills", []) if isinstance(x, dict) and not x.get("paid")]
                schedule = [x for x in content.get("familySchedule", []) if isinstance(x, dict)]
                schedule.sort(key=lambda x: (str(x.get("date") or "9999"), str(x.get("time") or "")))
                if mode == "family-express-bills":
                    return render_family_express_bills(express, bills, w, h, colors)
                if mode == "family-weather":
                    # 无天气数据源时降级为文字看板，保证出图
                    weather = cfg_root.get("_runtime_weather") or {}
                    rows = [f"{weather.get('city') or '本地'} · {weather.get('weather_str', '--')}",
                            str(weather.get("description") or "当前天气")]
                    return render_scene_modules("家庭天气" if mode == "family-weather" else "家庭提醒",
                                                rows, w, h, colors, str(weather.get("advice") or "实时天气"))
                if mode == "family-todos":
                    rows = [str(x.get('text') or '').strip() for x in content.get('homework', []) if isinstance(x, dict) and not x.get('done')]
                    rows += [str(x.get('text') or '').strip() for x in content.get('notes', []) if isinstance(x, dict) and str(x.get('text') or '').strip()]
                    rows = rows[:6] or ["暂无家庭待办"]
                    footer = f"待办 {len(rows)} 项"
                elif mode == "family-shopping":
                    rows = [str(x.get('text') or '') for x in content.get('familyShopping', []) if isinstance(x, dict) and not x.get('done')][:6]
                    rows = rows or ["采购清单为空"]
                    footer = ""
                elif mode == "family-anniversary":
                    events = [x for x in content.get('familyAnniversaries', []) if isinstance(x, dict)]
                    events.sort(key=lambda x: str(x.get('date') or '9999'))
                    rows = [f"{x.get('date', '')} {x.get('title', '')}" for x in events[:6]] or ["暂无纪念日"]
                    footer = f"共 {len(events)} 个纪念日"
                else:
                    if mode == "family":
                        weather = cfg_root.get("_runtime_weather") or {}
                        todos = [x.get("text", "") for x in content.get("homework", []) if isinstance(x, dict) and not x.get("done")]
                        shopping = [x.get("text", "") for x in content.get("familyShopping", []) if isinstance(x, dict) and not x.get("done")]
                        anniversaries = [f"{x.get('date', '')} {x.get('title', '')}" for x in content.get("familyAnniversaries", []) if isinstance(x, dict)]
                        rows = [f"天气 {weather.get('weather_str', '--')}"]
                        rows += [f"取件码 {x.get('pickupCode') or x.get('trackingNo') or '待补'} · {x.get('pickupPoint') or x.get('item') or '包裹'}" for x in express[:2]]
                        rows += [f"待缴 {x.get('name') or x.get('category') or '账单'} · ¥{x.get('amount', 0)}" for x in bills[:1]]
                        rows += [f"待办 {x}" for x in todos[:2]]
                        rows += [f"日程 {x.get('date', '')} {x.get('time', '')} {x.get('title', '')}" for x in schedule[:2]]
                        rows += [f"采购 {x}" for x in shopping[:1]]
                        rows += [f"纪念日 {x}" for x in anniversaries[:1]]
                        rows = rows[:6]
                        footer = f"家庭数据总览 · 日程 {len(schedule)} 项"
                    else:
                        rows = list(modules)
                        footer = "家庭共享提醒"
                return render_scene_modules("家庭提醒" if mode == "family" else (a_cfg.get("title") or "家庭提醒"), rows, w, h, colors, footer)
            # 理财场景：A 屏自选行情 / 市场摘要（实时拉取东财公开接口，失败回落通用看板）
            if scene_key == "finance" and mode in ("finance-watch", "finance-summary", "finance-market"):
                try:
                    from . import finance as _finance
                    return _finance.render_a_screen(a_cfg, cfg_root, w, h, mac=mac)
                except Exception as exc:  # noqa: BLE001
                    logger.warning("[FINANCE] A screen fallback: %s", exc)
            if scene_key in ("family", "health", "finance", "office", "custom") and mode not in ("notes", "homework", "reading", "album"):
                modules = a_cfg.get("modules") or []
                footer = ""
                if scene_key == "family":
                    content = cfg_root.get("content") or {}
                    express = [x for x in content.get("express", []) if isinstance(x, dict) and x.get("status") not in ("signed", "exception")]
                    bills = [x for x in content.get("bills", []) if isinstance(x, dict) and not x.get("paid")]
                    footer = f"未签收快递 {len(express)} · 待缴费 {len(bills)}"
                elif scene_key == "finance":
                    footer = "行情数据源未配置"
                elif scene_key == "health":
                    footer = "请按个人情况安排健康计划"
                return render_scene_modules(a_cfg.get("title") or "家庭提醒", modules, w, h, colors, footer)
            if mode != "album" and not _a_mode_has_data(mode, dual_config) and photos:
                return render_album(mac, w, h, colors, dual_config)
            if mode == "homework":
                return render_homework(mac, w, h, dual_config, battery=battery)
            if mode == "reading":
                return render_reading(mac, w, h, dual_config, battery=battery)
            if mode == "album":
                return render_album(mac, w, h, colors, dual_config)
            return render_notes(mac, w, h, dual_config, battery=battery)
        # B 屏
        cfg_root = dual_config or {}
        scene_key = str(cfg_root.get("sceneKey") or "study")
        scene_cfg = (cfg_root.get("config") or {}).get(scene_key) or {}
        b_cfg = scene_cfg.get("b") or {}
        # 未配置设备的出厂默认：B 屏展示成都天气（A 屏由固件欢迎页引导去后台配置）。
        b_mode = str(b_cfg.get("mode") or ("weather" if scene_key == "study" else "family" if scene_key == "family" else "health-plan" if scene_key == "health" else "finance-trend" if scene_key == "finance" else "office"))
        if b_mode == "published-app":
            return _render_published_app(b_cfg, w, h, colors, screen="B")
        if b_mode == "album":
            return render_album(mac, w, h, colors, dual_config)
        if scene_key == "family" and b_mode in ("calendar", "family"):
            content = cfg_root.get("content") or {}
            events = [x for x in content.get("familySchedule", []) if isinstance(x, dict)]
            events.sort(key=lambda x: (str(x.get('date') or '9999'), str(x.get('time') or '')))
            rows = [f"{x.get('date', '')} {x.get('time', '')} {x.get('title', '')}" for x in events[:6]]
            if b_mode == "family":
                rows = rows[:3] + [str(x) for x in (b_cfg.get("modules") or []) if str(x).strip()][:3]
            return render_scene_modules("家庭共享看板" if b_mode == "family" else "家庭日程", rows or ["暂无家庭日程"], w, h, colors, f"共 {len(events)} 项安排")
        # 理财场景：B 屏行情趋势 → 首只自选的日 K 蜡烛图（失败回落通用看板）
        if scene_key == "finance" and b_mode in ("finance-trend", "finance-kline", "finance-sectors"):
            try:
                from . import finance as _finance
                return _finance.render_b_screen(b_cfg, cfg_root, w, h, colors, mac=mac)
            except Exception as exc:  # noqa: BLE001
                logger.warning("[FINANCE] B screen fallback: %s", exc)
        # 天气场景：B 屏默认（未配置设备）展示天气，城市默认成都，可在 b_cfg.city 覆盖
        if b_mode == "weather":
            try:
                from . import console_api as _console
                city = str(b_cfg.get("city") or "成都")
                weather = _console._weather_payload(city)
                return render_weather(weather, w, h, colors)
            except Exception as exc:  # noqa: BLE001
                logger.warning("[WEATHER] B screen fallback: %s", exc)
        if scene_key != "study" or b_mode != "course":
            footer = ""
            if scene_key == "finance":
                footer = "行情数据源未配置"
            elif scene_key == "health":
                footer = "计划仅供日常提醒"
            modules = b_cfg.get("modules") or []
            return render_scene_modules(b_cfg.get("title") or "场景看板", modules, w, h, colors, footer)
        rows, _ = _collect_today_courses(dual_config, datetime.now())
        if not rows and _album_photos(dual_config):
            return render_album(mac, w, h, colors, dual_config)
        return render_course(mac, w, h, colors, dual_config)
    except Exception as exc:  # noqa: BLE001
        logger.error("[DUAL-RENDER] screen=%s failed: %s", screen, exc, exc_info=True)
        scale = w / 400.0
        img = _palette_p_image(w, h, colors) if colors >= 3 else Image.new("1", (w, h), 1)
        draw = ImageDraw.Draw(img)
        draw.text((int(14 * scale), int(70 * scale)),
                  "课程表/便签渲染失败", fill=0, font=_load_cjk(max(13, int(16 * scale))))
        return img


def _wrap_text(draw: ImageDraw.ImageDraw, text: str, font, max_w: int) -> list[str]:
    """按像素宽度换行，支持显式换行符。"""
    lines: list[str] = []
    for paragraph in str(text).split("\n"):
        cur = ""
        for ch in paragraph:
            test = cur + ch
            if _text_w(draw, test, font) > max_w and cur:
                lines.append(cur)
                cur = ch
            else:
                cur = test
        lines.append(cur)
    return lines


# ── 轨2：数据接口卡片（ISV 端点实时取数，后端渲染成 e-ink 卡片）──────────
_DATA_CACHE: dict[tuple[str, str], tuple[float, dict]] = {}
_DATA_TTL = 60.0


def _fetch_live_data(app_id: str, endpoint: str, token: str, screen: str) -> Optional[dict]:
    """带 60s 内存缓存的实时取数；成功时顺手回写样本，失败返回 None。"""
    key = (app_id, "B" if str(screen).upper() == "B" else "A")
    now = time.time()
    cached = _DATA_CACHE.get(key)
    if cached and now - cached[0] < _DATA_TTL:
        return cached[1]
    try:
        payload = data_api.fetch_card(endpoint, token, screen)
    except Exception as exc:  # noqa: BLE001
        logger.warning("[DATA-API] %s 取数失败：%s", app_id, exc)
        return None
    normalized = data_api.normalize_card(payload)
    _DATA_CACHE[key] = (now, normalized)
    try:
        store.update_data_source_sample(app_id, normalized)
    except Exception:  # noqa: BLE001
        pass
    return normalized


def _resolve_data_payload(app_id: str, ds: dict, screen: str) -> Optional[dict]:
    """实时优先、样本兜底；两者皆无则返回 None（上层回落海报卡）。"""
    endpoint = str(ds.get("endpoint") or "").strip()
    if endpoint:
        live = _fetch_live_data(app_id, endpoint, str(ds.get("token") or ""), screen)
        if live:
            return live
    sample = ds.get("sample")
    if isinstance(sample, dict) and (sample.get("title") or sample.get("items")):
        return data_api.normalize_card(sample)
    return None


def _render_data_card(payload: dict, w: int, h: int, colors: int) -> Image.Image:
    """按数据接口契约渲染一张 e-ink 卡片：标题/副标题/键值项/进度条/页脚/二维码。"""
    scale = w / 400.0
    is_color = colors >= 3
    if is_color:
        img = _palette_p_image(w, h, colors)
        fg, red, yellow = 0, 3, 2
    else:
        img = Image.new("1", (w, h), 1)
        fg = red = yellow = 0
    draw = ImageDraw.Draw(img)
    margin = int(16 * scale)

    title = payload.get("title") or "数据卡片"
    subtitle = payload.get("subtitle") or ""
    items = payload.get("items") or []
    footer = payload.get("footer") or ""
    qr = payload.get("qr") or ""

    font_title = _load_cjk(max(18, int(22 * scale)), bold=True)
    font_sub = _load_cjk(max(11, int(13 * scale)))
    font_label = _load_cjk(max(12, int(15 * scale)))
    font_value = _load_cjk(max(12, int(15 * scale)), bold=True)
    font_footer = _load_cjk(max(10, int(12 * scale)))

    y = int(12 * scale)
    draw.text((margin, y), _ellipsis_text(draw, title, font_title, w - 2 * margin - int(64 * scale)),
              fill=fg, font=font_title)
    hy = y + int(font_title.size * 1.28)
    if subtitle:
        draw.text((margin, hy), _ellipsis_text(draw, subtitle, font_sub, w - 2 * margin), fill=fg, font=font_sub)
        hy += int(font_sub.size * 1.4)
    draw.line([(margin, hy), (w - margin, hy)], fill=fg, width=max(1, int(1.5 * scale)))
    body_top = hy + int(10 * scale)

    footer_h = int(20 * scale) if footer else 0
    qr_size = int(60 * scale) if qr else 0
    body_bottom = h - margin - footer_h - (qr_size + int(6 * scale) if qr_size else 0)
    count = max(1, len(items))
    row_h = max(int(24 * scale), (body_bottom - body_top) // count)

    for i, item in enumerate(items):
        ry = body_top + i * row_h
        label = str(item.get("label") or "")
        value = str(item.get("value") or "")
        emphasis = str(item.get("emphasis") or "none")
        vcolor = fg
        if emphasis == "red" and is_color:
            vcolor = red
        elif emphasis == "yellow" and colors >= 4:
            vcolor = yellow
        draw.text((margin, ry), _ellipsis_text(draw, label, font_label, int(w * 0.5)), fill=fg, font=font_label)
        vw = _text_w(draw, value, font_value)
        draw.text((w - margin - vw, ry), value, fill=vcolor, font=font_value)
        prog = item.get("progress")
        if prog is not None:
            bar_y = ry + int(font_label.size * 1.5)
            bh = max(3, int(5 * scale))
            draw.rectangle([margin, bar_y, w - margin, bar_y + bh], outline=fg, width=1)
            fill_w = int((w - 2 * margin) * max(0, min(100, int(prog))) / 100)
            if fill_w > 0:
                draw.rectangle([margin, bar_y, margin + fill_w, bar_y + bh], fill=vcolor)
        elif i < len(items) - 1:
            sy = ry + row_h - int(4 * scale)
            draw.line([(margin, sy), (w - margin, sy)], fill=fg, width=1)

    if footer:
        draw.text((margin, h - margin - int(font_footer.size * 1.15)),
                  _ellipsis_text(draw, footer, font_footer,
                                 w - 2 * margin - (qr_size + int(8 * scale) if qr_size else 0)),
                  fill=fg, font=font_footer)
    if qr and str(qr).lower().startswith(("http://", "https://")):
        try:
            import qrcode  # 可选依赖：未安装时跳过
            ql = qrcode.QRCode(box_size=2, border=1, error_correction=qrcode.constants.ERROR_CORRECT_M)
            ql.add_data(qr)
            ql.make(fit=True)
            qimg = ql.make_image(fill_color="black", back_color="white").convert("L")
            qimg = qimg.resize((qr_size, qr_size)).point(lambda p: 255 if p > 128 else 0)
            img.paste(qimg, (w - margin - qr_size, h - margin - qr_size), qimg)
        except Exception:  # noqa: BLE001
            pass
    return img


def _render_published_app(cfg: dict, w: int, h: int, colors: int, screen: str = "A") -> Image.Image:
    """开发者已上线应用：服务端出图（黑白 / 彩色自适应）。

    - 若该应用配置了「数据接口」（轨2），优先实时取数并渲染数据卡片；失败时用样本兜底。
    - 否则渲染海报卡：应用名 + 场景标签 + 简介 + 入口地址；入口为绝对地址时附二维码，
      便于手机扫码打开完整交互应用；相对地址（如 /sandbox/...）则直接显示路径。
    """
    scale = w / 400.0
    is_color = colors >= 3
    if is_color:
        img = _palette_p_image(w, h, colors)
        fg = 0
        accent = 3  # 红
    else:
        img = Image.new("1", (w, h), 1)
        fg = 0
        accent = 0
    draw = ImageDraw.Draw(img)
    margin = int(16 * scale)

    app_id = cfg.get("appId") or ""
    app = None
    if app_id:
        try:
            app = store.get_published_app(app_id)
        except Exception:  # noqa: BLE001
            app = None

    # 轨2：应用配置了数据接口 → 实时取数渲染数据卡片（失败用样本兜底）
    if app_id:
        try:
            ds = store.get_app_data_source(app_id)
        except Exception:  # noqa: BLE001
            ds = None
        if ds:
            payload = _resolve_data_payload(app_id, ds, screen)
            if payload:
                return _render_data_card(payload, w, h, colors)

    name = cfg.get("title") or (app["name"] if app else "已上线应用")
    scene = app["scene"] if app else (cfg.get("scene") or cfg.get("source") or "")
    desc = (app.get("description") or "") if app else (cfg.get("description") or "")
    preview_url = (app.get("preview_url") or "") if app else (cfg.get("source") or "")

    font_title = _load_cjk(max(18, int(26 * scale)), bold=True)
    font_meta = _load_cjk(max(12, int(15 * scale)))
    font_body = _load_cjk(max(12, int(15 * scale)))

    title_y = int(16 * scale)
    draw.text((margin, title_y), _ellipsis_text(draw, name, font_title, w - 2 * margin), fill=fg, font=font_title)

    tag_y = title_y + int(font_title.size * 1.35)
    if scene:
        draw.text((margin, tag_y), f"#{scene}", fill=accent, font=font_meta)

    body_y = tag_y + int(font_meta.size * 1.7)
    if desc:
        for i, ln in enumerate(_wrap_text(draw, desc, font_body, w - 2 * margin)[:5]):
            draw.text((margin, body_y + i * int(font_body.size * 1.4)), ln, fill=fg, font=font_body)

    footer_y = h - int(44 * scale)
    if preview_url and str(preview_url).lower().startswith("http"):
        try:
            import qrcode  # 可选依赖：未安装时降级为文字
            qr_l = qrcode.QRCode(box_size=2, border=1, error_correction=qrcode.constants.ERROR_CORRECT_M)
            qr_l.add_data(preview_url)
            qr_l.make(fit=True)
            qr_img = qr_l.make_image(fill_color="black", back_color="white").convert("L")
            qsize = int(64 * scale)
            qr_img = qr_img.resize((qsize, qsize)).point(lambda p: 255 if p > 128 else 0)
            qr_x = w - margin - qsize
            draw.text((margin, footer_y - int(6 * scale)), "手机扫码打开", fill=fg, font=font_meta)
            img.paste(qr_img, (qr_x, int(footer_y - 12 * scale)), qr_img)
        except Exception:  # noqa: BLE001
            draw.text((margin, footer_y), _ellipsis_text(draw, f"打开：{preview_url}", font_meta, w - 2 * margin), fill=fg, font=font_meta)
    else:
        label = "开发者已上线应用" if not preview_url else f"入口：{preview_url}"
        draw.text((margin, footer_y), _ellipsis_text(draw, label, font_meta, w - 2 * margin), fill=fg, font=font_meta)
    return img


def render_dual_screen(mac: str, screen: str, w: int, h: int, colors: int,
                       dual_config: Optional[dict],
                       battery: Optional[int] = None,
                       unread_messages: int = 0) -> Image.Image:
    """渲染双屏画面，并在有未读留言时叠加提醒徽标。"""
    img = _render_dual_screen_inner(mac, screen, w, h, colors, dual_config, battery=battery)
    try:
        return _draw_message_badge(img, int(unread_messages or 0), colors)
    except Exception as exc:  # noqa: BLE001
        logger.warning("[DUAL-RENDER] message badge failed: %s", exc)
        return img


def render_error(mac: str, screen_w: int, screen_h: int,
                 colors: int = 2) -> Image.Image:
    img = _new_canvas(screen_w, screen_h, colors)
    draw = ImageDraw.Draw(img)
    font = _load_cjk(max(14, int(18 * scale_of(screen_w))))
    text = "渲染失败"
    draw.text((max(0, (screen_w - _text_w(draw, text, font)) // 2),
               int(screen_h * 0.42)), text, fill=0, font=font)
    return img


def render_unbound(mac: str, screen_w: int, screen_h: int, pair_code: str,
                   colors: int = 2) -> Image.Image:
    """设备未绑定：欢迎 + 配对码引导页（居中排版，黑白屏与彩色屏同一套版式）。

    A 屏（1bit BMP）和 B 屏（2bpp 裸帧）共用本函数，画布必须按目标屏选择：
    colors>=3 时走调色板图，否则白底会被打包成红色（见 _new_canvas）。
    """
    scale = scale_of(screen_w)
    img = _new_canvas(screen_w, screen_h, colors)
    draw = ImageDraw.Draw(img)

    font_title = _load_serif(max(18, int(23 * scale)))
    font_code = _load_serif(max(20, int(26 * scale)), bold=True)
    font_hint = _load_serif(max(12, int(14 * scale)))

    title = "欢迎使用 XiaoWuAI"
    code_line = f"配对码：{pair_code}" if pair_code else "正在生成配对码…"
    hint = "请在设备配置页输入配对码完成绑定"

    # (文本, 字体, 纵向位置比例)：三行垂直居中，保持与黑白屏一致的分层。
    rows = (
        (title, font_title, 0.30),
        (code_line, font_code, 0.45),
        (hint, font_hint, 0.64),
    )
    for text, font, ratio in rows:
        text_w = _text_w(draw, text, font)
        draw.text((max(0, (screen_w - text_w) // 2), int(screen_h * ratio)),
                  text, fill=0, font=font)
    return img


def render_unregistered(screen_w: int, screen_h: int,
                        colors: int = 2) -> Image.Image:
    """无 MAC（未配网/未注册）：展示重新配网引导。"""
    scale = scale_of(screen_w)
    img = _new_canvas(screen_w, screen_h, colors)
    draw = ImageDraw.Draw(img)
    font_title = _load_serif(max(16, int(22 * scale)))
    font_body = _load_serif(max(12, int(16 * scale)))
    rows = (
        ("设备未注册", font_title, 0.34),
        ("请先完成配网与注册", font_body, 0.54),
    )
    for text, font, ratio in rows:
        text_w = _text_w(draw, text, font)
        draw.text((max(0, (screen_w - text_w) // 2), int(screen_h * ratio)),
                  text, fill=0, font=font)
    return img


def scale_of(w: int) -> float:
    return w / 400.0
