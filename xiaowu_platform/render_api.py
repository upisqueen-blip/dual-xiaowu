"""设备取图 / 浏览器预览接口。

- GET /api/render   固件拉取双屏画面（DUAL_A=黑白 BMP，DUAL_B=彩色 2bpp 原始帧）。
- GET /api/preview  浏览器快照预览（返回 PNG），供内容后台「预览」按钮使用。

固件契约（firmware-dual 双屏板）：
- A 屏：GET /api/render?w=400&h=300&v=3.2[&persona=…][&screen=A]  + X-Device-Token → 1-bit BMP
- B 屏：GET /api/render?w=400&h=300&v=3.2&bpp=2&colors=3|4[&screen=B] → raw 2bpp
  裸帧判定以「请求里显式带了 bpp / colors」或 screen=B 为准，不再依赖 persona 命名：
  persona 只是看板模式（WEATHER / MY_ADAPTIVE …），默认空串；早先以 persona==DUAL_B
  判断裸帧，导致空 persona 的彩色屏拿到 BMP 被当裸帧解析（表现为屏幕一片红 +
  固件报 stream end early got=15662/30000）。
- 未绑定：返回带配对码的引导图，并置 X-Device-Unbound: 1
- 未注册（无 MAC）：返回重新配网引导图，并置 X-Device-Unregistered: no-mac
"""
from datetime import UTC, datetime
from typing import Annotated, Optional

from fastapi import APIRouter, Header, Query, Response
from fastapi.responses import JSONResponse

from .render import (
    image_to_bmp_bytes,
    image_to_png_bytes,
    image_to_raw_2bpp,
    image_to_raw_4bpp,
    render_dual_screen,
    render_error,
    render_unbound,
    render_unregistered,
)
from .settings import settings
from .store import Store

store = Store(settings.database_path)
router = APIRouter(tags=["render"])


def _pack_raw(img, colors: int, bpp: int) -> bytes:
    """按面板色数打包原始帧：六色 + bpp>=4 走 4bpp，其余 2bpp。"""
    if bpp >= 4 and colors >= 6:
        return image_to_raw_4bpp(img)
    return image_to_raw_2bpp(img)


@router.get("/api/render")
def render(
    mac: Optional[str] = Query(default=None),
    persona: Optional[str] = Query(default=None),
    w: int = Query(default=400, ge=100, le=1600),
    h: int = Query(default=300, ge=100, le=1200),
    colors: Optional[int] = Query(default=None, ge=2, le=6),
    bpp: Optional[int] = Query(default=None, ge=1, le=4),
    screen: Optional[str] = Query(default=None),
    v: Optional[float] = Query(default=None),
    rssi: Optional[int] = Query(default=None),
    battery: Optional[int] = Query(default=None),
    # 固件 A 屏请求历史上发的是 bat=（见 inksight_client.cc 的 Load），
    # 后端只认 battery，导致黑白屏右上角电量一直画不出来。两个名字都收。
    bat: Optional[int] = Query(default=None),
    x_device_token: Optional[str] = Header(default=None),
):
    # 裸帧（raw）判定：固件走原始帧时会显式带 bpp/colors（见 Load2bpp 与 7.3 寸
    # raw 路径），网页/单色屏预览都不带。screen 参数是显式声明，优先级最高。
    want_raw = bpp is not None or colors is not None
    requested_screen = str(screen or "").strip().upper()
    if requested_screen in ("A", "B"):
        screen = requested_screen
    elif str(persona or "").upper() == "DUAL_B" or want_raw:
        screen = "B"
    else:
        screen = "A"
    if battery is None and bat is not None:
        battery = bat
    is_color = screen == "B"  # B 屏恒为彩色原始帧
    bpp = bpp or 2
    colors = colors or 4
    start_time = datetime.now(UTC)

    # 1) 无 MAC：未注册引导图
    if not mac:
        guide = render_unregistered(w, h, colors if is_color else 2)
        if is_color:
            return Response(content=_pack_raw(guide, colors, bpp), media_type="application/octet-stream",
                            headers={"X-Device-Unregistered": "no-mac"})
        return Response(content=image_to_bmp_bytes(guide), media_type="image/bmp",
                        headers={"X-Device-Unregistered": "no-mac"})

    mac = mac.strip().upper()
    # 2) 设备未注册（无令牌可领）→ 重新配网引导
    if not store.device_exists(mac):
        guide = render_unregistered(w, h, colors if is_color else 2)
        if is_color:
            return Response(content=_pack_raw(guide, colors, bpp), media_type="application/octet-stream",
                            headers={"X-Device-Unregistered": "no-mac"})
        return Response(content=image_to_bmp_bytes(guide), media_type="image/bmp",
                        headers={"X-Device-Unregistered": "no-mac"})

    # 3) 令牌校验
    if not (x_device_token and store.verify_device_token(mac, x_device_token)):
        return Response(content=b"", status_code=401, media_type="application/octet-stream",
                        headers={"X-Error": "invalid-device-token"})

    # 顺手记一条心跳（固件只拉图、不 POST /heartbeat，避免网页显示离线）
    try:
        store.heartbeat(mac, {"firmware_version": "render", "battery_voltage": v, "rssi": rssi,
                              "runtime_mode": "active"})
    except Exception:  # noqa: BLE001
        pass

    owner = store.get_device_owner(mac)
    # 4) 未绑定 → 带配对码引导图
    if owner is None:
        pair_code = store.ensure_claim_code(mac)
        guide = render_unbound(mac, w, h, pair_code,
                               colors if is_color else 2)
        # X-Pair-Code 供固件打日志 / 排障用（屏幕上是图，日志里要能读到码）
        headers = {"X-Device-Unbound": "1", "X-Pair-Code": pair_code}
        if is_color:
            return Response(content=_pack_raw(guide, colors, bpp), media_type="application/octet-stream", headers=headers)
        return Response(content=image_to_bmp_bytes(guide), media_type="image/bmp", headers=headers)

    # 5) 已绑定 → 渲染双屏内容
    cfg_row = store.get_content_config(mac)
    dual_cfg = cfg_row["config"] if cfg_row else None
    # 未读留言数：设备画面据此叠加「留言 N」徽标，播放后自动消失
    try:
        unread_messages = store.count_unread_messages(mac)
    except Exception:  # noqa: BLE001
        unread_messages = 0

    eff_colors = colors
    if is_color:
        screen_cfg = (dual_cfg or {}).get("screen") or {}
        cfg_colors = screen_cfg.get("colors")
        if isinstance(cfg_colors, int) and cfg_colors in (2, 3, 4):
            eff_colors = min(cfg_colors, colors)

    try:
        if screen == "A":
            img = render_dual_screen(mac, "A", w, h, 2, dual_cfg, battery=battery,
                                     unread_messages=unread_messages)
            out_bytes = image_to_bmp_bytes(img)
            out_media = "image/bmp"
        else:
            img = render_dual_screen(mac, "B", w, h, eff_colors, dual_cfg, battery=None,
                                     unread_messages=unread_messages)
            out_bytes = _pack_raw(img, eff_colors, bpp)
            out_media = "application/octet-stream"
    except Exception as exc:  # noqa: BLE001
        img = render_error(mac, w, h, colors if is_color else 2)
        if is_color:
            out_bytes = _pack_raw(img, colors, bpp)
            out_media = "application/octet-stream"
        else:
            out_bytes = image_to_bmp_bytes(img)
            out_media = "image/bmp"

    elapsed_ms = int((datetime.now(UTC) - start_time).total_seconds() * 1000)
    headers = {
        "X-Render-Time-Ms": str(elapsed_ms),
        "X-Mode-Id": str(persona or ""),
        "X-Refresh-Minutes": "5" if screen == "A" else "30",
    }
    if store.consume_pending_refresh(mac):
        headers["X-Pending-Refresh"] = "1"
    return Response(content=out_bytes, media_type=out_media, headers=headers)


@router.get("/api/preview")
def preview(
    mac: Optional[str] = Query(default=None),
    persona: Optional[str] = Query(default=None),
    w: int = Query(default=400, ge=100, le=1600),
    h: int = Query(default=300, ge=100, le=1200),
    colors: int = Query(default=4, ge=2, le=6),
    bpp: int = Query(default=2, ge=1, le=4),
    sceneKey: Optional[str] = Query(default=None),
    aMode: Optional[str] = Query(default=None),
    bMode: Optional[str] = Query(default=None),
    appId: Optional[str] = Query(default=None),
    x_device_token: Optional[str] = Header(default=None),
):
    """浏览器快照预览：返回 PNG，便于在内容后台/小程序直接核对设备将显示的内容。

    支持即时预览覆盖参数 aMode/bMode/sceneKey：无需保存设备配置，直接按所选屏模式渲染，
    供内容后台「双屏配置」下拉切换模式时即时预览（与设备渲染同源，所见即所得）。

    支持 appId：开发者提交前 / 运营审核前，按「已上线应用 / 沙箱应用」解析出名称、简介、
    场景与入口，渲染该应用在海报卡模式下的真实出图（A 黑白 / B 四色），提前核对设备显示。
    """
    screen = "B" if str(persona or "").upper() == "DUAL_B" else "A"

    # 应用出图预览：按 appId 渲染「已上线应用」海报卡（与固件同源）
    if appId:
        info = store.get_published_app(appId) or store.sandbox_app_raw(appId)
        if not info:
            img = render_error("DEMO", w, h)
            return Response(content=image_to_png_bytes(img), media_type="image/png",
                            headers={"X-Preview-App": "not-found"})
        name = info.get("name") or "已上线应用"
        desc = info.get("description") or ""
        scene = info.get("scene") or "office"
        preview_url = info.get("preview_url") or ""
        if not preview_url:
            build = store.get_sandbox_build(appId)
            preview_url = (build or {}).get("preview_url") or f"/sandbox/{appId}/"
        sk = (sceneKey or scene).strip() or "office"
        override_cfg = {
            "version": 1,
            "sceneKey": sk,
            "screen": {"size": "4.2", "colors": colors},
            "config": {
                sk: {
                    "a": {"mode": "published-app", "appId": appId, "title": name, "description": desc,
                          "scene": scene, "source": preview_url},
                    "b": {"mode": "published-app", "appId": appId, "title": name, "description": desc,
                          "scene": scene, "source": preview_url},
                }
            },
        }
        try:
            if screen == "B":
                img = render_dual_screen("DEMO", "B", w, h, colors, override_cfg)
            else:
                img = render_dual_screen("DEMO", "A", w, h, 2, override_cfg)
        except Exception:  # noqa: BLE001
            img = render_error("DEMO", w, h)
        return Response(content=image_to_png_bytes(img), media_type="image/png",
                        headers={"X-Preview-App": appId, "X-Cache-Hit": "0"})

    # 即时预览覆盖：按所选模式直接出图（不依赖已保存的设备配置）
    if aMode or bMode or sceneKey:
        sk = (sceneKey or "finance").strip() or "finance"
        override_cfg = {
            "version": 1,
            "sceneKey": sk,
            "screen": {"size": "4.2", "colors": colors},
            "config": {
                sk: {
                    "a": {"mode": (aMode or "finance-watch"), "title": (aMode or "实时行情")},
                    "b": {"mode": (bMode or "finance-trend"), "title": (bMode or "行情趋势")},
                }
            },
        }
        try:
            if screen == "B":
                img = render_dual_screen("DEMO", "B", w, h, colors, override_cfg)
            else:
                img = render_dual_screen("DEMO", "A", w, h, 2, override_cfg)
        except Exception:  # noqa: BLE001
            img = render_error("DEMO", w, h)
        return Response(content=image_to_png_bytes(img), media_type="image/png",
                        headers={"X-Mode-Id": str(persona or ""), "X-Cache-Hit": "0", "X-Preview-Override": "1"})
    if mac:
        mac = mac.strip().upper()
        dual_cfg = None
        unread_messages = 0
        cfg_row = store.get_content_config(mac)
        if cfg_row:
            dual_cfg = cfg_row["config"]
        try:
            unread_messages = store.count_unread_messages(mac)
        except Exception:  # noqa: BLE001
            unread_messages = 0
        # 未绑定也允许预览（展示空态/相册兜底），无需设备令牌
        eff_colors = colors
        if screen == "B":
            screen_cfg = (dual_cfg or {}).get("screen") or {}
            cfg_colors = screen_cfg.get("colors")
            if isinstance(cfg_colors, int) and cfg_colors in (2, 3, 4):
                eff_colors = min(cfg_colors, colors)
            img = render_dual_screen(mac, "B", w, h, eff_colors, dual_cfg,
                                     unread_messages=unread_messages)
        else:
            img = render_dual_screen(mac, "A", w, h, 2, dual_cfg,
                                     unread_messages=unread_messages)
        return Response(content=image_to_png_bytes(img), media_type="image/png",
                        headers={"X-Mode-Id": str(persona or ""), "X-Cache-Hit": "0"})
    # 无 mac：渲染一张演示图（默认学习场景）
    demo_cfg = _demo_config()
    if screen == "B":
        img = render_dual_screen("DEMO", "B", w, h, colors, demo_cfg)
    else:
        img = render_dual_screen("DEMO", "A", w, h, 2, demo_cfg)
    return Response(content=image_to_png_bytes(img), media_type="image/png",
                    headers={"X-Mode-Id": str(persona or ""), "X-Cache-Hit": "0"})


def _demo_config() -> dict:
    """无设备时的演示双屏配置，方便打开预览即有内容。"""
    return {
        "version": 1,
        "sceneKey": "study",
        "screen": {"size": "4.2", "colors": 4},
        "config": {
            "study": {
                "a": {"mode": "notes", "title": "便签"},
                "b": {"mode": "course", "title": "课程表"},
            }
        },
        "content": {
            "notes": [
                {"text": "今天记得带跳绳", "sender": "家长", "sender_type": "parent", "date": _today()},
                {"text": "数学作业订正第 3 页", "sender": "老师", "sender_type": "teacher", "date": _today()},
            ],
            "courseTable": {
                "enabled": True, "school": "森穹小学", "class_name": "三年级二班",
                "periods": [
                    {"start_hour": 8, "start_minute": 0, "end_hour": 8, "end_minute": 40},
                    {"start_hour": 8, "start_minute": 50, "end_hour": 9, "end_minute": 30},
                    {"start_hour": 10, "start_minute": 0, "end_hour": 10, "end_minute": 40},
                    {"start_hour": 14, "start_minute": 0, "end_hour": 14, "end_minute": 40},
                ],
                "grid": [_weekday_grid()],
            },
        },
    }


def _today() -> str:
    return datetime.now().strftime("%Y-%m-%d")


def _weekday_grid() -> list[dict]:
    sample = ["语文", "数学", "英语", "体育"]
    return [{"name": sample[i % len(sample)], "teacher": "王老师", "room": f"{i+1}01"} for i in range(4)]
