"""用户内容后台（dual 控制台）全部接口。

前端（apps/content-console-cn，经 next.config 的 /api 反代）调用这里的 /api/* 端点，
复用既有 users / device_bindings / content_configs 体系，新增少量表支撑共享成员、
老师授权、个人大模型配置、孩子信息、服务设置与图片上传。
"""
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Annotated, Any
import json
import logging
import os
import re
import secrets
import time
import urllib.request
import urllib.parse

from fastapi import APIRouter, Depends, File, Form, Header, HTTPException, Query, UploadFile, status
from fastapi.responses import JSONResponse, Response
from pydantic import BaseModel, Field

from .settings import settings
from .store import Store, now_iso
from . import finance as _finance
from . import render as _render

store = Store(settings.database_path)
router = APIRouter()
logger = logging.getLogger(__name__)

# 双屏固件按彩屏间隔（默认 30 分钟）拉图/上报心跳，窗口必须大于刷新周期，
# 否则两次心跳之间设备会被误判离线。45min = 30min 周期 + 15min 余量。
ONLINE_WINDOW = timedelta(minutes=45)


def current_user(
    x_user_token: Annotated[str | None, Header()] = None,
    authorization: Annotated[str | None, Header()] = None,
) -> str:
    token = x_user_token
    if not token and authorization and authorization.lower().startswith("bearer "):
        token = authorization.split(" ", 1)[1].strip()
    if not token:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "未登录")
    user_id = store.session_user(token)
    if not user_id:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "登录已失效，请重新登录")
    return user_id


def can_manage_device(user_id: str, mac: str) -> bool:
    """设备所有者本人，或平台管理员，都可管理该设备。

    管理员账号（admin_accounts）不参与 device_bindings，如果只按「是否绑定」
    校验，管理员登录后既看不到设备、也存不了配置。这里统一放开。
    """
    return store.user_owns(user_id, mac) or store.is_platform_admin(user_id)


def optional_user(
    x_user_token: Annotated[str | None, Header()] = None,
    authorization: Annotated[str | None, Header()] = None,
) -> str | None:
    token = x_user_token
    if not token and authorization and authorization.lower().startswith("bearer "):
        token = authorization.split(" ", 1)[1].strip()
    if not token:
        return None
    return store.session_user(token)


def online(last_seen: str | None) -> bool:
    if not last_seen:
        return False
    try:
        return datetime.fromisoformat(last_seen.replace("Z", "+00:00")) > datetime.now(UTC) - ONLINE_WINDOW
    except ValueError:
        return False


# ——— 认证 ———
class RegisterIn(BaseModel):
    username: str
    password: str
    email: str = ""
    phone: str = ""
    phone_region: str = "CN"


class LoginIn(BaseModel):
    username: str
    password: str


@router.post("/api/auth/register")
def auth_register(body: RegisterIn) -> dict[str, Any]:
    try:
        store.register_user(body.username, body.password)
    except ValueError as e:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, str(e))
    return {"ok": True, "message": "注册成功"}


@router.post("/api/auth/login")
def auth_login(body: LoginIn) -> dict[str, Any]:
    result = store.login(body.username, body.password)
    if not result:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "用户名或密码错误")
    user_id, token = result
    return {"user_id": user_id, "token": token}


@router.get("/api/auth/me")
def auth_me(user_id: str = Depends(current_user)) -> dict[str, Any]:
    with store.connect() as db:
        row = db.execute("SELECT username FROM users WHERE user_id=?", (user_id,)).fetchone()
        if not row:
            row = db.execute("SELECT username FROM admin_accounts WHERE admin_id=?", (user_id,)).fetchone()
    return {"user_id": user_id, "username": row["username"] if row else user_id}


# —— 找回密码（演示环境无 SMTP，验证码直接在响应中返回）——
_reset_codes: dict[str, tuple[str, float]] = {}  # ident(lower) -> (code, expire_ts)
_RESET_CODE_TTL = 600  # 10 分钟


def _resolve_account(db, ident: str) -> dict[str, Any] | None:
    """按 邮箱或用户名 在 admin / users / developer 三张表里定位账号。"""
    i = (ident or "").strip().lower()
    if not i:
        return None
    # 管理员：admin_accounts 只有 username（无 email 列）
    row = db.execute(
        "SELECT admin_id AS id, username, 'admin' AS kind FROM admin_accounts WHERE lower(username)=?",
        (i,),
    ).fetchone()
    if row:
        return {k: row[k] for k in row.keys()}
    # 普通用户：users.username
    row = db.execute(
        "SELECT user_id AS id, username, 'user' AS kind FROM users WHERE lower(username)=?",
        (i,),
    ).fetchone()
    if row:
        return {k: row[k] for k in row.keys()}
    # 开发者：developer_accounts.email 或 name
    row = db.execute(
        "SELECT developer_id AS id, email AS username, 'developer' AS kind FROM developer_accounts "
        "WHERE lower(email)=? OR lower(name)=?",
        (i, i),
    ).fetchone()
    if row:
        return {k: row[k] for k in row.keys()}
    return None


@router.post("/api/auth/reset-password/send-code")
def reset_send(body: dict[str, Any]) -> dict[str, Any]:
    ident = (body.get("email") or "").strip()
    if not ident:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "请输入注册邮箱或登录用户名")
    with store.connect() as db:
        acct = _resolve_account(db, ident)
        if not acct:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "账号不存在")
    code = f"{secrets.randbelow(1000000):06d}"
    _reset_codes[ident.lower()] = (code, time.time() + _RESET_CODE_TTL)
    return {
        "ok": True,
        "message": f"验证码（演示环境未发信，请直接填入）：{code}",
        "dev_code": code,
    }


@router.post("/api/auth/reset-password")
def reset_do(body: dict[str, Any]) -> dict[str, Any]:
    ident = (body.get("email") or "").strip()
    code = (body.get("code") or "").strip()
    password = body.get("password") or ""
    if not ident or not code or len(password) < 4:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "参数不完整")
    entry = _reset_codes.get(ident.lower())
    if not entry or entry[1] < time.time():
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "验证码无效或已过期")
    if entry[0] != code:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "验证码错误")
    with store.connect() as db:
        acct = _resolve_account(db, ident)
        if not acct:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "账号不存在")
        salt = secrets.token_hex(16)
        phash = store._password_hash(password, salt)
        if acct["kind"] == "admin":
            db.execute(
                "UPDATE admin_accounts SET password_hash=?, password_salt=? WHERE admin_id=?",
                (phash, salt, acct["id"]),
            )
        elif acct["kind"] == "user":
            db.execute(
                "UPDATE users SET password_hash=?, password_salt=? WHERE user_id=?",
                (phash, salt, acct["id"]),
            )
        else:  # developer
            db.execute(
                "UPDATE developer_accounts SET password_hash=?, password_salt=? WHERE developer_id=?",
                (phash, salt, acct["id"]),
            )
    _reset_codes.pop(ident.lower(), None)
    return {"ok": True, "message": "密码已重置"}


# ——— 设备 ———
@router.get("/api/user/devices")
def user_devices(user_id: str = Depends(current_user)) -> dict[str, Any]:
    """设备列表。

    - 普通用户：只返回自己绑定/被共享的设备；
    - 平台管理员：返回全平台设备（带归属账号，未绑定设备也能看到，便于运维排查）。
    """
    is_admin = store.is_platform_admin(user_id)
    rows = store.all_devices_with_owner() if is_admin else store.user_devices(user_id)
    devices = [
        {
            "mac": r["device_id"],
            "nickname": r.get("nickname") or "",
            "role": "admin" if is_admin else "owner",
            "status": "online" if online(r["last_seen_at"]) else "offline",
            "is_online": online(r["last_seen_at"]),
            "last_seen": r["last_seen_at"] or "暂无记录",
            "model": r.get("model"),
            "owner": r.get("owner_username"),
        }
        for r in rows
    ]
    return {"devices": devices, "scope": "all" if is_admin else "mine"}


class BindIn(BaseModel):
    mac: str
    nickname: str = ""


@router.post("/api/user/devices")
def user_bind(body: BindIn, user_id: str = Depends(current_user)) -> dict[str, Any]:
    mac = body.mac.strip().upper()
    try:
        store.bind_device(user_id, mac, body.nickname or "我的小悟双屏")
    except ValueError as e:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, str(e))
    return {"ok": True, "mac": mac}


@router.delete("/api/user/devices/{mac}")
def user_unbind(mac: str, user_id: str = Depends(current_user)) -> dict[str, Any]:
    store.unbind_device(user_id, mac.strip().upper())
    return {"ok": True}


class ShareIn(BaseModel):
    username: str


@router.post("/api/user/devices/{mac}/share")
def user_share(mac: str, body: ShareIn, user_id: str = Depends(current_user)) -> dict[str, Any]:
    try:
        store.share_device(user_id, mac.strip().upper(), body.username.strip(), "member")
    except ValueError as e:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, str(e))
    return {"ok": True}


@router.get("/api/user/devices/{mac}/members")
def user_members(mac: str, user_id: str = Depends(current_user)) -> dict[str, Any]:
    rows = store.device_members(mac.strip().upper())
    members = [
        {"user_id": r["user_id"], "username": r["username"], "role": "owner"}
        for r in rows
    ]
    return {"members": members}


@router.delete("/api/user/devices/{mac}/members/{target_user_id}")
def user_remove_member(mac: str, target_user_id: str, user_id: str = Depends(current_user)) -> dict[str, Any]:
    mac = mac.strip().upper()
    if not store.user_owns(user_id, mac):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "你不是该设备的所有者")
    if target_user_id == user_id:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "不能移除设备所有者本人")
    store.remove_member(mac, target_user_id.strip())
    return {"ok": True}


class ClaimIn(BaseModel):
    pair_code: str


@router.post("/api/claim/consume")
def claim(body: ClaimIn, user_id: str = Depends(current_user)) -> dict[str, Any]:
    try:
        device_id = store.consume_claim(body.pair_code, user_id, "我的小悟双屏")
    except ValueError as e:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, str(e))
    return {"ok": True, "mac": device_id}


# ——— 双屏内容配置 ———
def _factory_system_config(mac: str) -> dict[str, Any]:
    """出厂默认双屏配置：系统场景（A 屏欢迎页 + B 屏本地天气）。

    新设备刚绑定、用户还没在后台保存过任何内容时，dual-config 返回它，
    固件才会判定「已启用双屏」并开始拉图——否则屏幕上只有固件内置欢迎页，
    B 屏永远不亮。用户一旦保存，就以真实配置覆盖（本函数不落库）。
    """
    return {
        "version": 1,
        "sceneKey": "system",
        "deviceMac": mac,
        "screen": {"size": "4.2", "colors": 3},
        "config": {
            "system": {
                "a": {
                    "mode": "xiaowu-today", "title": "小悟今日", "source": "设备数据",
                    "refresh": "随双屏刷新",
                    "modules": ["日期星期", "今日安排", "电量", "尾号"],
                },
                "b": {
                    "mode": "weather", "title": "本地天气",
                    "source": "和风天气（按城市）", "refresh": "每 30 分钟",
                    "modules": ["实时天气", "三日预报", "日出日落", "生活提示"],
                },
            }
        },
        "content": {},
        "savedAt": "factory-system-v1",
    }


@router.get("/api/device/{mac}/dual-config")
def get_dual_config(
    mac: str,
    x_device_token: Annotated[str | None, Header()] = None,
    user_id: str | None = Depends(optional_user),
) -> dict[str, Any]:
    mac = mac.strip().upper()
    # 固件 LoadDualConfig 用 X-Device-Token；Web/小程序用用户归属
    if x_device_token and store.verify_device_token(mac, x_device_token):
        pass
    elif user_id and can_manage_device(user_id, mac):
        pass
    else:
        raise HTTPException(status.HTTP_403_FORBIDDEN, "无权访问该设备配置")
    cfg = store.get_content_config(mac)
    if not cfg:
        # 未配置 = 出厂状态，走系统场景默认（不写库，避免把默认值当成用户数据）。
        return {"config": _factory_system_config(mac), "updated_at": "factory-system-v1"}
    return cfg


@router.post("/api/device/{mac}/dual-config")
def post_dual_config(mac: str, body: dict[str, Any], user_id: str = Depends(current_user)) -> dict[str, Any]:
    # 契约兼容：内容后台历来直发整个配置对象（{version,sceneKey,config,content,...}），
    # 小程序/第三方按 {"payload": {...}} 包装。两种都要收，否则前端会拿到 422。
    mac = mac.strip().upper()
    if not isinstance(body, dict):
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, "配置格式不正确")
    inner = body.get("payload")
    payload = inner if isinstance(inner, dict) else body
    payload.pop("payload", None)
    if not can_manage_device(user_id, mac):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "设备未绑定到当前账号")
    # 理财自选股保护：整包覆盖时若 payload 未携带 financeWatchlist，保留服务端已有值
    existing = store.get_content_config(mac)
    incoming_watch = (payload.get("content") or {}).get("financeWatchlist")
    if not incoming_watch and existing:
        old_watch = (existing.get("content") or {}).get("financeWatchlist")
        if old_watch:
            payload.setdefault("content", {})["financeWatchlist"] = old_watch
    ts = store.save_content_config(mac, payload)
    # 「应用到设备」的语义就是立即生效：置位 pending_refresh，固件下一次取图
    # 会收到 X-Pending-Refresh: 1 并立刻刷屏（不再等 5/30 分钟周期）。
    try:
        store.set_pending_refresh(mac, True)
    except Exception:  # noqa: BLE001
        pass
    store.audit(user_id, "dual-config.save", mac, {"keys": list(payload.keys())})
    return {"ok": True, "updated_at": ts}


# ——— 已上线应用目录（供内容后台选择并配置到双屏）———
@router.get("/api/published-apps")
def published_apps_for_console(user_id: str = Depends(current_user)) -> dict[str, Any]:
    apps = store.list_published_apps(enabled_only=True)
    return {
        "apps": [
            {
                "app_id": a["app_id"],
                "name": a["name"],
                "description": a.get("description") or "",
                "scene": a["scene"],
                "preview_url": a["preview_url"],
            }
            for a in apps
        ]
    }


class SceneCreateIn(BaseModel):
    name: str = Field(min_length=1, max_length=40)
    eyebrow: str = Field(default="", max_length=20)
    desc: str = Field(default="", max_length=120)
    icon: str = Field(default="", max_length=4)
    scene_key: str = Field(default="", max_length=40)


@router.get("/api/scenes")
def list_user_scenes_for_console(user_id: str = Depends(current_user)) -> dict[str, Any]:
    scenes = store.list_user_scenes(user_id)
    return {"scenes": [
        {
            "scene_key": s["scene_key"], "name": s["name"], "eyebrow": s.get("eyebrow") or "",
            "desc": s.get("desc") or "", "icon": s.get("icon") or "自",
            "created_at": s.get("created_at"), "updated_at": s.get("updated_at"),
        }
        for s in scenes
    ]}


def _scene_slug(text: str) -> str:
    out = re.sub(r"[^0-9A-Za-z\u4e00-\u9fff]+", "-", text or "").strip("-")
    return out or "scene"


@router.post("/api/scenes")
def create_user_scene_for_console(body: SceneCreateIn, user_id: str = Depends(current_user)) -> dict[str, Any]:
    name = body.name.strip()
    if not name:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "场景名称不能为空")
    base = body.scene_key.strip() if body.scene_key else _scene_slug(name)
    base = re.sub(r"[^0-9A-Za-z\u4e00-\u9fff_-]", "-", base).strip("-") or "scene"
    existing = {s["scene_key"] for s in store.list_user_scenes(user_id)}
    key = base
    while key in existing:
        key = f"{base}-{secrets.token_hex(3)}"
    store.create_user_scene(user_id, key, name, body.eyebrow.strip(), body.desc.strip(), body.icon.strip() or "自")
    s = store.get_user_scene(user_id, key)
    return {"scene": {
        "scene_key": s["scene_key"], "name": s["name"], "eyebrow": s.get("eyebrow") or "",
        "desc": s.get("desc") or "", "icon": s.get("icon") or "自",
        "created_at": s.get("created_at"), "updated_at": s.get("updated_at"),
    }}


class SceneUpdateIn(BaseModel):
    name: str = Field(default="", max_length=40)
    eyebrow: str = Field(default="", max_length=20)
    desc: str = Field(default="", max_length=120)
    icon: str = Field(default="", max_length=4)


@router.put("/api/scenes/{scene_key}")
def update_user_scene_for_console(scene_key: str, body: SceneUpdateIn, user_id: str = Depends(current_user)) -> dict[str, Any]:
    if not store.get_user_scene(user_id, scene_key):
        raise HTTPException(status.HTTP_404_NOT_FOUND, "场景不存在")
    fields = {k: v for k, v in body.model_dump().items() if v != ""}
    if not store.update_user_scene(user_id, scene_key, fields):
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "没有可更新的字段")
    s = store.get_user_scene(user_id, scene_key)
    return {"scene": {
        "scene_key": s["scene_key"], "name": s["name"], "eyebrow": s.get("eyebrow") or "",
        "desc": s.get("desc") or "", "icon": s.get("icon") or "自",
    }}


@router.delete("/api/scenes/{scene_key}")
def delete_user_scene_for_console(scene_key: str, user_id: str = Depends(current_user)) -> dict[str, Any]:
    if not store.delete_user_scene(user_id, scene_key):
        raise HTTPException(status.HTTP_404_NOT_FOUND, "场景不存在")
    return {"deleted": scene_key}


@router.post("/api/lightacade/live-ticket")
async def lightacade_live_ticket() -> dict[str, Any]:
    """LightAcade 口语陪练票据接口（固件首页「口语陪练」菜单项调用）。

    本地/自托管后端未接入 LightAcade 合作方服务（真实票据由 aioneschool.com
    发放）。未配置时返回明确的 503 而非 404，避免固件日志出现难以排查的
    "Ticket endpoint HTTP 404"。生产环境若已采购合作方服务，应在此代理到
    POST /api/partner/v1/speech/live-tickets 并返回 {ticket, expiresInSeconds}。
    """
    raise HTTPException(
        status.HTTP_503_SERVICE_UNAVAILABLE,
        "LightAcade partner service not configured in this environment",
    )


# 注：固件取图接口 GET /api/render 统一由 render_api.py 提供（含设备绑定判定、
# X-Device-Unbound / X-Pair-Code 响应头、空 persona 容错）。此处不再重复定义，
# 否则会因路由注册顺序覆盖固件真正需要的实现，导致空 persona 请求 404。


# ——— 天气预览（演示数据，方便配置界面查看效果）———
_SOLAR_TERMS_21C: list[tuple[str, float]] = [
    # 21 世纪寿星公式 C 值，按 (小寒, 大寒, 立春, ...) 排列，每年 1-12 月各两个
    ("小寒", 5.4055), ("大寒", 20.12), ("立春", 3.87), ("雨水", 18.73),
    ("惊蛰", 5.63), ("春分", 20.646), ("清明", 4.81), ("谷雨", 20.1),
    ("立夏", 5.52), ("小满", 21.04), ("芒种", 5.678), ("夏至", 21.37),
    ("小暑", 7.108), ("大暑", 22.83), ("立秋", 7.5), ("处暑", 23.13),
    ("白露", 7.646), ("秋分", 23.042), ("寒露", 8.318), ("霜降", 23.438),
    ("立冬", 7.438), ("小雪", 22.36), ("大雪", 7.18), ("冬至", 21.94),
]


def _solar_term_info(now: datetime | None = None) -> dict[str, str]:
    """计算当前节气与处于该节气的第几天（21 世纪寿星公式，误差 ≤1 天，演示足够）。"""
    now = now or datetime.now(UTC).astimezone()
    y = now.year % 100
    anchors: list[tuple[datetime, str]] = []
    for offset_years in (0, 1):  # 覆盖跨年（冬至/小寒）
        year = now.year + offset_years
        yy = year % 100
        for idx, (name, c) in enumerate(_SOLAR_TERMS_21C):
            month = idx // 2 + 1
            day = int(yy * 0.2422 + c) - int(yy / 4) if yy else int(c)
            try:
                anchors.append((datetime(year, month, day), name))
            except ValueError:
                continue
    past = [a for a in anchors if a[0].date() <= now.date()]
    if not past:
        return {"solar_term": "—", "solar_term_day": "1"}
    term_date, term_name = past[-1]
    return {"solar_term": term_name, "solar_term_day": str((now.date() - term_date.date()).days + 1)}


_CITY_PINYIN: dict[str, str] = {
    "北京": "BEIJING", "上海": "SHANGHAI", "广州": "GUANGZHOU", "深圳": "SHENZHEN",
    "杭州": "HANGZHOU", "成都": "CHENGDU", "重庆": "CHONGQING", "武汉": "WUHAN",
    "西安": "XIAN", "南京": "NANJING", "苏州": "SUZHOU", "天津": "TIANJIN",
    "长沙": "CHANGSHA", "郑州": "ZHENGZHOU", "合肥": "HEFEI", "福州": "FUZHOU",
    "厦门": "XIAMEN", "昆明": "KUNMING", "贵阳": "GUIYANG", "青岛": "QINGDAO",
}

# 农历压缩表（1900-2100，每年一个十六进制值），经典万年历算法
_LUNAR_INFO: list[int] = [
    0x04BD8, 0x04AE0, 0x0A570, 0x054D5, 0x0D260, 0x0D950, 0x16554, 0x056A0, 0x09AD0, 0x055D2,
    0x04AE0, 0x0A5B6, 0x0A4D0, 0x0D250, 0x1D255, 0x0B540, 0x0D6A0, 0x0ADA2, 0x095B0, 0x14977,
    0x04970, 0x0A4B0, 0x0B4B5, 0x06A50, 0x06D40, 0x1AB54, 0x02B60, 0x09570, 0x052F2, 0x04970,
    0x06566, 0x0D4A0, 0x0EA50, 0x06E95, 0x05AD0, 0x02B60, 0x186E3, 0x092E0, 0x1C8D7, 0x0C950,
    0x0D4A0, 0x1D8A6, 0x0B550, 0x056A0, 0x1A5B4, 0x025D0, 0x092D0, 0x0D2B2, 0x0A950, 0x0B557,
    0x06CA0, 0x0B550, 0x15355, 0x04DA0, 0x0A5B0, 0x14573, 0x052B0, 0x0A9A8, 0x0E950, 0x06AA0,
    0x0AEA6, 0x0AB50, 0x04B60, 0x0AAE4, 0x0A570, 0x05260, 0x0F263, 0x0D950, 0x05B57, 0x056A0,
    0x096D0, 0x04DD5, 0x04AD0, 0x0A4D0, 0x0D4D4, 0x0D250, 0x0D558, 0x0B540, 0x0B6A0, 0x195A6,
    0x095B0, 0x049B0, 0x0A974, 0x0A4B0, 0x0B27A, 0x06A50, 0x06D40, 0x0AF46, 0x0AB60, 0x09570,
    0x04AF5, 0x04970, 0x064B0, 0x074A3, 0x0EA50, 0x06B58, 0x05AC0, 0x0AB60, 0x096D5, 0x092E0,
    0x0C960, 0x0D954, 0x0D4A0, 0x0DA50, 0x07552, 0x056A0, 0x0ABB7, 0x025D0, 0x092D0, 0x0CAB5,
    0x0A950, 0x0B4A0, 0x0BAA4, 0x0AD50, 0x055D9, 0x04BA0, 0x0A5B0, 0x15176, 0x052B0, 0x0A930,
    0x07954, 0x06AA0, 0x0AD50, 0x05B52, 0x04B60, 0x0A6E6, 0x0A4E0, 0x0D260, 0x0EA65, 0x0D530,
    0x05AA0, 0x076A3, 0x096D0, 0x04AFB, 0x04AD0, 0x0A4D0, 0x1D0B6, 0x0D250, 0x0D520, 0x0DD45,
    0x0B5A0, 0x056D0, 0x055B2, 0x049B0, 0x0A577, 0x0A4B0, 0x0AA50, 0x1B255, 0x06D20, 0x0ADA0,
    0x14B63, 0x09370, 0x049F8, 0x04970, 0x064B0, 0x168A6, 0x0EA50, 0x06B20, 0x1A6C4, 0x0AAE0,
    0x0A2E0, 0x0D2E3, 0x0C960, 0x0D557, 0x0D4A0, 0x0DA50, 0x05D55, 0x056A0, 0x0A6D0, 0x055D4,
    0x052D0, 0x0A9B8, 0x0A950, 0x0B4A0, 0x0B6A6, 0x0AD50, 0x055A0, 0x0ABA4, 0x0A5B0, 0x052B0,
    0x0B273, 0x06930, 0x07337, 0x06AA0, 0x0AD50, 0x14B55, 0x04B60, 0x0A570, 0x054E4, 0x0D160,
    0x0E968, 0x0D520, 0x0DAA0, 0x16AA6, 0x056D0, 0x04AE0, 0x0A9D4, 0x0A2D0, 0x0D150, 0x0F252,
    0x0D520,
]
_LUNAR_MONTH_NAMES = ["正月", "二月", "三月", "四月", "五月", "六月", "七月", "八月", "九月", "十月", "冬月", "腊月"]
_LUNAR_DAY_NAMES = ["初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
                    "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
                    "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"]
_STEMS = "甲乙丙丁戊己庚辛壬癸"
_BRANCHES = "子丑寅卯辰巳午未申酉戌亥"
_ZODIACS = "鼠牛虎兔龙蛇马羊猴鸡狗猪"


def _lunar_leap_month(y: int) -> int:
    return _LUNAR_INFO[y - 1900] & 0xF


def _lunar_leap_days(y: int) -> int:
    if _lunar_leap_month(y):
        return 30 if _LUNAR_INFO[y - 1900] & 0x10000 else 29
    return 0


def _lunar_month_days(y: int, m: int) -> int:
    return 30 if _LUNAR_INFO[y - 1900] & (0x10000 >> m) else 29


def _lunar_year_days(y: int) -> int:
    total, info = 348, _LUNAR_INFO[y - 1900]
    bit = 0x8000
    while bit > 0x8:
        total += 1 if info & bit else 0
        bit >>= 1
    return total + _lunar_leap_days(y)


def _lunar_date_info(now: datetime) -> dict[str, str]:
    """公历 → 农历（月日 / 干支年 / 生肖）。支持 1900-2100。"""
    from datetime import date as _date
    offset = (now.date() - _date(1900, 1, 31)).days  # 1900-01-31 为农历 1900 年正月初一
    if offset < 0:
        return {}
    year, temp = 1900, 0
    while year < 2101 and offset > 0:
        temp = _lunar_year_days(year)
        offset -= temp
        year += 1
    if offset < 0:
        offset += temp
        year -= 1
    leap = _lunar_leap_month(year)
    is_leap, month, temp2 = False, 1, temp
    month = 1
    leap_passed = False
    while month < 13 and offset > 0:
        if leap > 0 and month == leap + 1 and not leap_passed:
            month -= 1
            leap_passed = True
            temp2 = _lunar_leap_days(year)
        else:
            temp2 = _lunar_month_days(year, month)
        if leap_passed and month == leap + 1:
            leap_passed = False
        offset -= temp2
        month += 1
    if offset == 0 and leap > 0 and month == leap + 1:
        if leap_passed:
            leap_passed = False
        else:
            leap_passed = True
            month -= 1
    if offset < 0:
        offset += temp2
        month -= 1
    month_label = ("闰" if leap_passed else "") + _LUNAR_MONTH_NAMES[month - 1]
    ganzhi = f"{_STEMS[(year - 4) % 10]}{_BRANCHES[(year - 4) % 12]}"
    zodiac = _ZODIACS[(year - 4) % 12]
    return {
        "lunar_date": f"{month_label}{_LUNAR_DAY_NAMES[offset]}",
        "lunar_year": f"{ganzhi}年",
        "lunar_zodiac": zodiac,
    }


def _weather_payload(city: str) -> dict[str, Any]:
    """天气预览数据。当前为演示数据，接入真实天气源时只需替换本函数。

    与设备解耦：预览只依赖城市，不需要先绑定/选中设备。
    """
    now = datetime.now(UTC).astimezone()
    # 尾号限行（通用工作日规则，可按城市扩展）：周一 1/6 ... 周五 5/0，周末不限行
    _PLATE_LIMITS = [["1", "6"], ["2", "7"], ["3", "8"], ["4", "9"], ["5", "0"]]
    weekday = now.weekday()
    plate_limit = " / ".join(_PLATE_LIMITS[weekday]) if weekday < 5 else "不限行"
    return {
        "city": city or "杭州",
        "city_pinyin": _CITY_PINYIN.get(city or "杭州", ""),
        **_solar_term_info(now),
        **_lunar_date_info(now),
        "plate_limit": plate_limit,
        "today_temp": "22°",
        "current_temp": "21°",
        "feels_like": "26°",
        "today_desc": "小雨",
        "today_code": 2,
        "today_low": "19°",
        "today_high": "24°",
        "today_humidity": "62%",
        "today_wind_dir": "东南风",
        "today_wind_level": "3 级",
        "sunrise": "06:12",
        "sunset": "17:48",
        "advice": "雨天出门，记得带伞。",
        "updated_at": now.strftime("%Y-%m-%d %H:%M"),
        # 预报日期必须跟着当天走：写死的日期（如 10-22）会在十一月露馅。
        # 数值仍是演示值，接真实源时连这里一起替换。
        "forecast": [
            {
                "day": "明天" if i == 1 else ("后天" if i == 2 else "周" + "一二三四五六日"[(now + timedelta(days=i)).weekday()]),
                "date": (now + timedelta(days=i)).strftime("%m-%d"),
                "temp_range": rng,
                "desc": desc,
                "code": code,
            }
            for i, desc, code, rng in (
                (1, "多云", 1, "19°~23°"),
                (2, "多云转晴", 2, "20°~26°"),
                (3, "小雨", 61, "18°~24°"),
            )
        ],
    }


@router.get("/api/weather")
def weather_preview(city: str = "", user_id: str = Depends(current_user)) -> dict[str, Any]:
    """天气预览（无需设备），供内容配置界面在还没有绑定设备时预览效果。"""
    return _weather_payload(city.strip())


@router.get("/api/device/{mac}/dual-weather")
def dual_weather(mac: str, city: str = "", user_id: str = Depends(current_user)) -> dict[str, Any]:
    """设备维度的天气预览。未传 city 时回落到该设备双屏配置里保存的城市。"""
    mac = mac.strip().upper()
    resolved_city = city.strip()
    if not resolved_city:
        cfg = store.get_content_config(mac) or {}
        payload = cfg.get("payload") or cfg.get("config") or {}
        resolved_city = str((payload.get("content") or {}).get("weatherCity") or payload.get("weatherCity") or "")
    return _weather_payload(resolved_city)


# ——— 理财场景：自选股管理 + 实时行情 + K 线（东财公开接口原型）———
def _parse_codes(raw: Any) -> list[str]:
    if isinstance(raw, str):
        parts = re.split(r"[\s,，;；]+", raw)
    elif isinstance(raw, list):
        parts = [str(x) for x in raw]
    else:
        parts = []
    out, seen = [], set()
    for p in parts:
        code = p.strip().lower()
        if re.match(r"^(sh|sz|bj)?\d{6}$", code) and code not in seen:
            seen.add(code)
            out.append(code)
    return out


def _user_watchlist(user_id: str) -> list[dict[str, Any]]:
    profile = store.get_user_profile(user_id, "finance") or {}
    return [x for x in profile.get("watchlist", []) if isinstance(x, dict) and x.get("code")]


@router.get("/api/finance/watchlist")
def finance_watchlist(user_id: str = Depends(current_user)) -> dict[str, Any]:
    return {"watchlist": _user_watchlist(user_id)}


class WatchlistIn(BaseModel):
    codes: Any


@router.post("/api/finance/watchlist")
def finance_watchlist_save(body: WatchlistIn, user_id: str = Depends(current_user)) -> dict[str, Any]:
    codes = _parse_codes(body.codes)
    if not codes:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "未识别到有效的 6 位证券代码")
    if len(codes) > 8:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "自选股最多 8 只（墨水屏版面限制）")
    # fetch_quotes 返回顺序与入参一致，按位对齐取名，避免 000001（平安银行）
    # 与 sh000001（上证指数）这类同码不同市的名称串号
    named = {c: q.get("name") or "" for c, q in zip(codes, _finance.fetch_quotes(codes))}
    watchlist = [{"code": c, "name": named.get(c, "")} for c in codes]
    _update_finance_profile(user_id, watchlist=watchlist)
    return {"ok": True, "watchlist": watchlist}


@router.delete("/api/finance/watchlist/{code}")
def finance_watchlist_delete(code: str, user_id: str = Depends(current_user)) -> dict[str, Any]:
    rest = [x for x in _user_watchlist(user_id) if str(x.get("code", "")).lower() != code.strip().lower()]
    _update_finance_profile(user_id, watchlist=rest)
    return {"ok": True, "watchlist": rest}


@router.get("/api/finance/quote")
def finance_quote(codes: str = "", user_id: str = Depends(current_user)) -> dict[str, Any]:
    parsed = _parse_codes(codes)
    if not parsed:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "缺少 codes 参数（如 600519,000001）")
    if len(parsed) > 20:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "单次最多查询 20 只")
    quotes = _finance.fetch_quotes(parsed)
    if not quotes:
        raise HTTPException(status.HTTP_502_BAD_GATEWAY, "行情数据获取失败（数据源不可用或代码无效）")
    return {"quotes": quotes, "source": "eastmoney", "updated_at": now_iso()}


@router.get("/api/finance/kline")
def finance_kline(code: str = "", days: int = 60, user_id: str = Depends(current_user)) -> dict[str, Any]:
    if not _parse_codes(code):
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "缺少有效的证券代码")
    return _finance.fetch_kline(code.strip(), days=days)


@router.get("/api/finance/kline/image")
def finance_kline_image(code: str = "", days: int = 60,
                        w: int = Query(default=400, ge=100, le=960),
                        h: int = Query(default=300, ge=100, le=680),
                        colors: int = Query(default=4, ge=2, le=6)) -> dict[str, Any]:
    """日 K 位图（PNG 预览）。与 B 屏渲染同一套逻辑，公开访问（仅公开行情数据）。"""
    from fastapi.responses import Response as FastAPIResponse
    from .render import image_to_png_bytes
    if not _parse_codes(code):
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "缺少有效的证券代码")
    kline = _finance.fetch_kline(code.strip(), days=days)
    img = _finance.render_kline_image(kline, w, h, colors)
    return FastAPIResponse(content=image_to_png_bytes(img), media_type="image/png")


class FinanceApplyIn(BaseModel):
    mac: str


@router.post("/api/finance/watchlist/apply")
def finance_watchlist_apply(body: FinanceApplyIn, user_id: str = Depends(current_user)) -> dict[str, Any]:
    """把当前账号的自选股写入设备 dual_config（content.financeWatchlist），设备下次唤醒生效。"""
    mac = body.mac.strip().upper()
    if not can_manage_device(user_id, mac):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "设备未绑定到当前账号")
    watchlist = _user_watchlist(user_id)
    if not watchlist:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "请先添加自选股")
    cfg = store.get_content_config(mac) or {"version": 1, "sceneKey": "finance", "config": {}, "content": {}}
    content = cfg.setdefault("content", {})
    content["financeWatchlist"] = watchlist
    cfg["content"] = content
    store.save_content_config(mac, cfg)
    store.set_pending_refresh(mac, True)
    store.audit(user_id, "finance.watchlist.apply", mac, {"count": len(watchlist)})
    return {"ok": True, "count": len(watchlist), "message": "已写入设备，下次唤醒刷新生效"}


# ——— 理财：推荐股 + 资金管理（持仓）———

def _update_finance_profile(user_id: str, **updates: Any) -> dict[str, Any]:
    """user_profiles(finance) 按键合并更新，避免 watchlist/portfolio 互相覆盖。"""
    profile = store.get_user_profile(user_id, "finance") or {}
    profile.update(updates)
    store.set_user_profile(user_id, "finance", profile)
    return profile


@router.get("/api/finance/gold")
def finance_gold(user_id: str = Depends(current_user)) -> dict[str, Any]:
    """黄金/期货实时快照（贵金属与商品期货主连）。"""
    return {"items": _finance.fetch_gold_futures(), "updated_at": now_iso()}


@router.get("/api/finance/search")
def finance_search(q: str = Query(min_length=1, max_length=20),
                   user_id: str = Depends(current_user)) -> dict[str, Any]:
    """证券代码/名称/拼音反查：持仓录入支持直接输股票名称。"""
    return {"items": _finance.search_security(q)}


@router.get("/api/finance/recommend")
def finance_recommend(top: int = Query(default=5, ge=1, le=8),
                      user_id: str = Depends(current_user)) -> dict[str, Any]:
    """智能推荐股：候选池当日动量排序（规则化，仅供参考）。"""
    quotes = _finance.fetch_quotes(_finance.RECO_POOL)
    return {
        "items": _finance.rank_recommendations(quotes, top=top),
        "disclaimer": _finance.RECO_DISCLAIMER,
        "updated_at": now_iso(),
    }


class PortfolioPositionIn(BaseModel):
    code: str
    name: str = ""
    shares: float = Field(gt=0)
    cost: float = Field(gt=0)


class PortfolioIn(BaseModel):
    cash: float = Field(default=0, ge=0)
    positions: list[PortfolioPositionIn]


def _portfolio_view(user_id: str, portfolio: dict[str, Any]) -> dict[str, Any]:
    codes = [str(p["code"]) for p in portfolio.get("positions", [])]
    quotes = _finance.fetch_quotes(codes) if codes else []
    return _finance.build_portfolio_view(portfolio.get("positions", []), quotes,
                                         cash=float(portfolio.get("cash") or 0))


@router.get("/api/finance/portfolio")
def finance_portfolio(user_id: str = Depends(current_user)) -> dict[str, Any]:
    """资金管理：持仓 × 实时行情 → 逐只盈亏/权重/调仓参考 + 汇总。"""
    profile = store.get_user_profile(user_id, "finance") or {}
    portfolio = {"cash": profile.get("cash") or 0, "positions": profile.get("portfolio") or []}
    view = _portfolio_view(user_id, portfolio)
    view["cash"] = portfolio["cash"]
    return view


@router.post("/api/finance/portfolio")
def finance_portfolio_save(body: PortfolioIn, user_id: str = Depends(current_user)) -> dict[str, Any]:
    if len(body.positions) > 10:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "持仓最多 10 只")
    positions = [p.model_dump() for p in body.positions]
    codes = [p["code"] for p in positions]
    named = {c: q.get("name") or "" for c, q in zip(codes, _finance.fetch_quotes(codes))}
    for p in positions:
        if not p["name"]:
            p["name"] = named.get(p["code"], "")
    _update_finance_profile(user_id, portfolio=positions, cash=body.cash)
    return {"ok": True, "count": len(positions)}


@router.post("/api/finance/portfolio/apply")
def finance_portfolio_apply(body: FinanceApplyIn, user_id: str = Depends(current_user)) -> dict[str, Any]:
    """把资金管理视图写入设备 dual_config（content.financePortfolio），下次唤醒生效。"""
    mac = body.mac.strip().upper()
    if not can_manage_device(user_id, mac):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "设备未绑定到当前账号")
    profile = store.get_user_profile(user_id, "finance") or {}
    portfolio = {"cash": profile.get("cash") or 0, "positions": profile.get("portfolio") or []}
    if not portfolio["positions"]:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "请先录入持仓")
    view = _portfolio_view(user_id, portfolio)
    cfg = store.get_content_config(mac) or {"version": 1, "sceneKey": "finance", "config": {}, "content": {}}
    content = cfg.setdefault("content", {})
    content["financePortfolio"] = view
    cfg["content"] = content
    store.save_content_config(mac, cfg)
    store.set_pending_refresh(mac, True)
    store.audit(user_id, "finance.portfolio.apply", mac, {"count": len(portfolio["positions"])})
    return {"ok": True, "count": len(portfolio["positions"]), "message": "已写入设备，下次唤醒刷新生效"}


# ——— 老师授权 ———
class TeacherGenIn(BaseModel):
    device_mac: str
    auth_name: str = "老师"
    validity_days: int = 30
    permission: str = "only_note"


@router.post("/api/teacher-auth/generate")
def teacher_generate(body: TeacherGenIn, user_id: str = Depends(current_user)) -> dict[str, Any]:
    mac = body.device_mac.strip().upper()
    if not can_manage_device(user_id, mac):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "设备未绑定到当前账号")
    row = store.create_teacher_code(mac, body.auth_name or "老师", body.permission, user_id, body.validity_days)
    return {"auth_code": row["auth_code"], "expire_time": row["expire_time"], "qr_text": f"XIAOWU-TEACHER:{row['auth_code']}"}


@router.get("/api/teacher-auth/list")
def teacher_list(mac: str, user_id: str = Depends(current_user)) -> dict[str, Any]:
    mac = mac.strip().upper()
    if not can_manage_device(user_id, mac):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "设备未绑定到当前账号")
    return {"items": store.list_teacher_codes(mac)}


class TeacherFreezeIn(BaseModel):
    device_mac: str
    auth_code: str
    frozen: int = 1


@router.post("/api/teacher-auth/freeze")
def teacher_freeze(body: TeacherFreezeIn, user_id: str = Depends(current_user)) -> dict[str, Any]:
    mac = body.device_mac.strip().upper()
    if not can_manage_device(user_id, mac):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "设备未绑定到当前账号")
    store.freeze_teacher_code(mac, body.auth_code.strip().upper(), body.frozen == 1)
    return {"ok": True}


# ——— 个人大模型配置 / 孩子信息 ———
@router.get("/api/user/profile")
def user_profile(user_id: str = Depends(current_user)) -> dict[str, Any]:
    return {"llm_config": store.get_user_profile(user_id, "llm") or {}}


@router.put("/api/user/profile/llm")
def put_llm(body: dict[str, Any], user_id: str = Depends(current_user)) -> dict[str, Any]:
    store.set_user_profile(user_id, "llm", body)
    return {"ok": True}


@router.get("/api/user/child")
def get_child(user_id: str = Depends(current_user)) -> dict[str, Any]:
    return {"child": store.get_user_profile(user_id, "child") or {}}


@router.put("/api/user/child")
def put_child(body: dict[str, Any], user_id: str = Depends(current_user)) -> dict[str, Any]:
    store.set_user_profile(user_id, "child", body)
    return {"ok": True}


# ——— 系统配置（服务设置，如快递100）———
@router.get("/api/settings/service")
def get_service() -> dict[str, Any]:
    return store.get_service_config()


class ServiceIn(BaseModel):
    key: str
    value: Any


@router.post("/api/settings/service")
def post_service(body: ServiceIn) -> dict[str, Any]:
    cfg = store.get_service_config()
    cfg[body.key] = body.value
    store.set_service_config(cfg)
    return {"ok": True}


# ——— 图片上传（相册）———
UPLOAD_DIR = settings.database_path.parent / "uploads"
ALLOWED = {".png", ".jpg", ".jpeg", ".webp", ".gif"}


@router.post("/api/uploads")
def upload(file: UploadFile = File(...), user_id: str = Depends(current_user)) -> dict[str, Any]:
    ext = Path(file.filename or "").suffix.lower()
    if ext not in ALLOWED:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "仅支持图片文件")
    UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
    import secrets
    fname = f"{secrets.token_hex(12)}{ext}"
    data = file.file.read()
    (UPLOAD_DIR / fname).write_bytes(data)
    return {"id": fname, "url": f"/uploads/{fname}"}


# ——— 留言（文本 / 语音）———
# 留言与设备绑定：一台设备一个留言箱。写入方可为 PC 后台 / 小程序（用户令牌），
# 也可为设备本地录音（设备令牌）；读出方按 played_at 判定是否已听过。
MESSAGE_AUDIO_DIR = UPLOAD_DIR / "messages"
MESSAGE_AUDIO_MIME = {
    ".wav": "audio/wav", ".mp3": "audio/mpeg", ".ogg": "audio/ogg", ".oga": "audio/ogg",
    ".m4a": "audio/mp4", ".aac": "audio/aac", ".opus": "audio/ogg", ".webm": "audio/webm",
    ".amr": "audio/amr", ".flac": "audio/flac",
}
MAX_AUDIO_BYTES = 8 * 1024 * 1024  # 单条语音 8MB 上限，够 4.2 寸屏场景


def _message_access(
    mac: str,
    x_device_token: Annotated[str | None, Header()] = None,
    user_id: str | None = Depends(optional_user),
) -> tuple[str, str]:
    """双通道鉴权：设备令牌（固件 / 本地录音）或用户归属（PC 后台 / 小程序）。

    返回 (mac, channel)，channel ∈ {"device", "user"}。
    """
    mac = (mac or "").strip().upper()
    if not mac:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "缺少设备标识")
    if x_device_token and store.verify_device_token(mac, x_device_token):
        return mac, "device"
    if user_id and can_manage_device(user_id, mac):
        return mac, "user"
    raise HTTPException(status.HTTP_403_FORBIDDEN, "无权访问该设备留言")


def _probe_duration(path: Path, ext: str, fallback: float) -> float:
    """尽量从文件头解析时长：WAV 走标准库精确计算，其余回落到客户端上报值。"""
    if fallback > 0:
        return round(float(fallback), 1)
    if ext == ".wav":
        try:
            import wave
            with wave.open(str(path), "rb") as wav:
                frames = wav.getnframes()
                rate = wav.getframerate() or 1
                return round(frames / float(rate), 1)
        except Exception:  # noqa: BLE001
            return 0.0
    return 0.0


class MessageCreateIn(BaseModel):
    kind: str = "text"
    text: str = ""
    sender_name: str = ""
    duration: float = 0
    audio_id: str = ""


@router.get("/api/device/{mac}/messages")
def list_messages(
    mac: str,
    limit: int = Query(default=50, ge=1, le=200),
    unread: bool = Query(default=False),
    x_device_token: Annotated[str | None, Header()] = None,
    user_id: str | None = Depends(optional_user),
) -> dict[str, Any]:
    mac_key, _ = _message_access(mac, x_device_token, user_id)
    items = store.list_messages(mac_key, limit, unread_only=unread)
    return {"items": items, "unread": store.count_unread_messages(mac_key), "count": len(items)}


@router.post("/api/device/{mac}/messages")
def create_message(
    mac: str,
    body: MessageCreateIn,
    x_device_token: Annotated[str | None, Header()] = None,
    user_id: str | None = Depends(optional_user),
) -> dict[str, Any]:
    mac_key, channel = _message_access(mac, x_device_token, user_id)
    kind = "voice" if body.kind == "voice" else "text"
    if kind == "voice" and not body.audio_id:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "语音留言缺少音频文件，请先上传")
    if kind == "text" and not body.text.strip():
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "文本留言内容不能为空")
    sender_name = body.sender_name.strip() or ("设备录音" if channel == "device" else "家人")
    item = store.create_message(
        mac_key, sender_name, kind, text=body.text, audio_id=body.audio_id,
        duration=body.duration, source="device" if channel == "device" else "console",
    )
    store.audit(user_id or f"device:{mac_key}", "message.create", mac_key, {"kind": kind, "id": item["id"]})
    return {"ok": True, "message": item, "unread": store.count_unread_messages(mac_key)}


@router.post("/api/device/{mac}/messages/voice")
async def upload_voice_message(
    mac: str,
    file: UploadFile = File(...),
    duration: float = Form(default=0),
    sender_name: str = Form(default=""),
    text: str = Form(default=""),
    x_device_token: Annotated[str | None, Header()] = None,
    user_id: str | None = Depends(optional_user),
) -> dict[str, Any]:
    """语音留言上传：浏览器 MediaRecorder（webm/ogg/mp3）或设备端本地录音均走这里。"""
    mac_key, channel = _message_access(mac, x_device_token, user_id)
    ext = Path(file.filename or "").suffix.lower()
    if ext not in MESSAGE_AUDIO_MIME:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, f"不支持的音频格式：{ext or '未知'}")
    MESSAGE_AUDIO_DIR.mkdir(parents=True, exist_ok=True)
    data = await file.read()
    if not data:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "音频为空")
    if len(data) > MAX_AUDIO_BYTES:
        raise HTTPException(status.HTTP_413_REQUEST_ENTITY_TOO_LARGE, "音频超过 8MB 上限")
    import secrets
    audio_id = f"{secrets.token_hex(12)}{ext}"
    path = MESSAGE_AUDIO_DIR / audio_id
    path.write_bytes(data)
    dur = _probe_duration(path, ext, duration)
    sender_name = sender_name.strip() or ("设备录音" if channel == "device" else "家人")
    item = store.create_message(
        mac_key, sender_name, "voice", text=text.strip()[:200], audio_id=audio_id,
        audio_mime=MESSAGE_AUDIO_MIME[ext], duration=dur,
        source="device" if channel == "device" else "console",
    )
    store.audit(user_id or f"device:{mac_key}", "message.voice.upload", mac_key,
                {"id": item["id"], "bytes": len(data), "duration": dur})
    return {"ok": True, "message": item, "unread": store.count_unread_messages(mac_key)}


@router.get("/api/messages/{message_id}/audio")
def message_audio(
    message_id: str,
    token: str = Query(default=""),
    x_device_token: Annotated[str | None, Header()] = None,
    user_id: str | None = Depends(optional_user),
) -> Response:
    """播放留言：返回音频二进制。鉴权三通道——Bearer/用户令牌、X-Device-Token、?token=（便于直接喂给播放器）。"""
    item = store.get_message(message_id)
    if not item or not item.get("audio_id"):
        raise HTTPException(status.HTTP_404_NOT_FOUND, "留言不存在或没有语音")
    uid = user_id
    if not uid and token:
        uid = store.session_user(token)
    ok = False
    if uid and can_manage_device(uid, item["device_id"]):
        ok = True
    if not ok and x_device_token and store.verify_device_token(item["device_id"], x_device_token):
        ok = True
    if not ok:
        raise HTTPException(status.HTTP_403_FORBIDDEN, "无权播放该留言")
    path = MESSAGE_AUDIO_DIR / str(item["audio_id"])
    if not path.is_file():
        raise HTTPException(status.HTTP_404_NOT_FOUND, "音频文件已丢失")
    return Response(
        content=path.read_bytes(),
        media_type=item.get("audio_mime") or "audio/mpeg",
        headers={
            "X-Message-Duration": str(item.get("duration") or 0),
            # HTTP 头必须是 latin-1，中文留言人用 URL 编码传递（客户端 quote() 回来）
            "X-Message-Sender": urllib.parse.quote(str(item.get("sender_name") or "")),
            "Accept-Ranges": "bytes",
        },
    )


@router.post("/api/device/{mac}/messages/{message_id}/played")
def mark_message_played(
    mac: str,
    message_id: str,
    x_device_token: Annotated[str | None, Header()] = None,
    user_id: str | None = Depends(optional_user),
) -> dict[str, Any]:
    mac_key, _ = _message_access(mac, x_device_token, user_id)
    store.mark_message_played(mac_key, message_id)
    return {"ok": True, "unread": store.count_unread_messages(mac_key)}


@router.post("/api/device/{mac}/messages/read-all")
def mark_all_messages_played(
    mac: str,
    x_device_token: Annotated[str | None, Header()] = None,
    user_id: str | None = Depends(optional_user),
) -> dict[str, Any]:
    mac_key, _ = _message_access(mac, x_device_token, user_id)
    changed = store.mark_all_messages_played(mac_key)
    return {"ok": True, "marked": changed, "unread": store.count_unread_messages(mac_key)}


@router.delete("/api/device/{mac}/messages/{message_id}")
def delete_message(
    mac: str,
    message_id: str,
    x_device_token: Annotated[str | None, Header()] = None,
    user_id: str | None = Depends(optional_user),
) -> dict[str, Any]:
    mac_key, _ = _message_access(mac, x_device_token, user_id)
    item = store.get_message(message_id)
    if item and item.get("audio_id"):
        try:
            (MESSAGE_AUDIO_DIR / str(item["audio_id"])).unlink(missing_ok=True)
        except Exception:  # noqa: BLE001
            pass
    ok = store.delete_message(mac_key, message_id)
    if not ok:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "留言不存在")
    return {"ok": True, "unread": store.count_unread_messages(mac_key)}


# ——— 快递追踪（演示实现，可在系统配置填入快递100 凭据后切换真实接口）———
@router.post("/api/express/track")
def express_track(body: dict[str, Any] = {}) -> dict[str, Any]:
    tracking = (body.get("trackingNumber") or body.get("tracking") or "").strip()
    if not tracking:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "缺少快递单号")
    cfg = store.get_service_config()
    if cfg.get("kuaidi100_key") and cfg.get("kuaidi100_customer"):
        # 真实接口需后端联调，这里返回结构占位，避免前端报错
        return {"ok": True, "trackingNumber": tracking, "carrier": "未知",
                "status": "已签收", "traces": [], "demo": True}
    # 演示数据
    return {
        "ok": True,
        "trackingNumber": tracking,
        "carrier": "顺丰速运",
        "status": "运输中",
        "demo": True,
        "traces": [
            {"time": "2026-10-20 09:12", "desc": "【成都】快件已发往 极客谷体验中心"},
            {"time": "2026-10-19 21:40", "desc": "【深圳】快件已到达 深圳转运中心"},
            {"time": "2026-10-19 14:05", "desc": "【深圳】顺丰已收取快件"},
        ],
    }


# ——— 作业 OCR（演示实现：接收图片，返回可编辑文本）———
@router.post("/api/ocr/homework")
async def ocr_homework(file: UploadFile = File(None), body: dict[str, Any] = {}) -> dict[str, Any]:
    # 无外部视觉模型时返回空文本，前端进入手动编辑模式，不阻断流程
    return {"ok": True, "text": "", "lines": [], "demo": True,
            "message": "演示环境未接入 OCR，请手动输入作业内容"}


# ——— 设备令牌（固件启动领取，供 X-Device-Token 鉴权）———
def _self_enroll(mac: str, body: dict[str, Any]) -> None:
    """设备自助入网登记。

    设备首次联网（或重新配网）时 devices 表里没有它，/token 会 404 → 固件拿不到
    X-Device-Token → /api/render 401 → 屏幕永远显示不出配对码，形成死循环。
    因此未知 MAC 在首次领取令牌时自动登记：建 devices 行 + 生成 6 位配对码，
    状态保持「未绑定」，由用户在内容后台输入配对码完成绑定。
    """
    from .models import DeviceCapabilities

    model = str((body or {}).get("model") or "xiaowu-s3-dual")
    firmware = str((body or {}).get("firmware_version") or "")
    capabilities = (body or {}).get("capabilities")
    if not isinstance(capabilities, dict):
        capabilities = DeviceCapabilities().model_dump()
    store.register(mac, model, firmware, capabilities)


@router.post("/api/device/{mac}/token")
def device_token(mac: str, body: dict[str, Any] = {}) -> dict[str, Any]:
    mac = mac.strip().upper()
    if not store.device_exists(mac):
        # 自助入网：XIAOWU_DEVICE_SELF_ENROLL=false 可关闭（产线由 /api/v1/devices/register 预登记）
        if os.getenv("XIAOWU_DEVICE_SELF_ENROLL", "true").lower() not in {"1", "true", "yes"}:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "设备未注册")
        try:
            _self_enroll(mac, body or {})
        except Exception as exc:  # noqa: BLE001
            if "UNIQUE" not in str(exc):
                raise
        if not store.device_exists(mac):
            raise HTTPException(status.HTTP_404_NOT_FOUND, "设备未注册")
    token = store.mint_device_token(mac)
    return {"token": token, "device_id": mac, "registered": True}


# ——— 设备解绑自身（固件 UnbindSelf）———
@router.post("/api/device/{mac}/unbind")
def device_unbind(mac: str, x_device_token: Annotated[str | None, Header()] = None) -> dict[str, Any]:
    mac = mac.strip().upper()
    if not (x_device_token and store.verify_device_token(mac, x_device_token)):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "设备令牌无效")
    store.unbind_device_self(mac)
    return {"ok": True, "message": "设备已解绑"}


# ——— 设备上报运行模式（固件 ReportRuntimeMode）———
@router.post("/api/device/{mac}/runtime")
def device_runtime(mac: str, body: dict[str, Any] = {}, x_device_token: Annotated[str | None, Header()] = None) -> dict[str, Any]:
    mac = mac.strip().upper()
    if not (x_device_token and store.verify_device_token(mac, x_device_token)):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "设备令牌无效")
    mode = (body.get("mode") or "interval")
    awake = bool(body.get("awake"))
    store.report_runtime(mac, mode, awake)
    # 只读不清：这里若 consume，会把「应用到设备」的立即刷新信号吃掉，
    # 随后真正取图的请求就没有 X-Pending-Refresh 了。
    refresh_required = store.peek_pending_refresh(mac)
    return {"ok": True, "refresh_required": refresh_required}


@router.post("/api/device/{mac}/storage")
def device_storage(mac: str, body: dict[str, Any] = {}, x_device_token: Annotated[str | None, Header()] = None) -> dict[str, Any]:
    """设备上报 TF 卡存储快照（本地录音 / 照片 / 小说 / 音频）。"""
    mac = mac.strip().upper()
    if not (x_device_token and store.verify_device_token(mac, x_device_token)):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "设备令牌无效")
    store.report_storage(mac, body or {})
    return {"ok": True}


@router.get("/api/device/{mac}/storage")
def get_device_storage(mac: str, user_id: str = Depends(current_user)) -> dict[str, Any]:
    """后台「TF 卡」面板读取设备最近一次上报的存储快照。"""
    mac = mac.strip().upper()
    if not store.user_owns(user_id, mac):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "无权访问该设备的 TF 卡")
    return {"storage": store.get_storage(mac)}


def _resolve_security_code(raw: str) -> tuple[str, str]:
    """把「乐鑫科技」/「688018」/「sh688018」解析成 (code, name)。"""
    from . import finance as _finance
    query = (raw or "").strip()
    if not query:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "缺少股票名称或代码")
    matched = re.fullmatch(r"(?:sh|sz|bj)?(\d{6})", query, flags=re.IGNORECASE)
    if matched:
        code = matched.group(1)
        quotes = _finance.fetch_quotes([code])
        name = (quotes[0].get("name") if quotes else "") or code
        return code, str(name)
    items = _finance.search_security(query) or []
    if not items:
        raise HTTPException(status.HTTP_404_NOT_FOUND, f"未找到证券：{query}")
    top = items[0]
    code = str(top.get("code") or "").strip()
    name = str(top.get("name") or code)
    if not code:
        raise HTTPException(status.HTTP_404_NOT_FOUND, f"未找到证券：{query}")
    return code, name


@router.post("/api/device/{mac}/finance/kline")
def device_finance_kline(mac: str, body: dict[str, Any] = {},
                         x_device_token: Annotated[str | None, Header()] = None) -> dict[str, Any]:
    """设备（语音端）自助把 B 屏切为某只证券的日 K：支持 6 位代码或中文名称。"""
    mac = mac.strip().upper()
    if not (x_device_token and store.verify_device_token(mac, x_device_token)):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "设备令牌无效")
    code, name = _resolve_security_code(str(body.get("query") or body.get("code") or ""))
    cfg = store.get_content_config(mac)
    payload = cfg["config"] if cfg else {}
    if not isinstance(payload, dict) or "sceneKey" not in payload:
        payload = {"version": 1, "sceneKey": "finance", "config": {}, "content": {},
                   "screen": {"size": "4.2", "colors": 4}}
    payload["sceneKey"] = "finance"
    scene_cfg = payload.setdefault("config", {}).setdefault("finance", {})
    scene_cfg["a"] = {**(scene_cfg.get("a") or {}), "mode": "finance-watch",
                      "title": (scene_cfg.get("a") or {}).get("title") or "实时行情"}
    scene_cfg["b"] = {**(scene_cfg.get("b") or {}), "mode": "finance-trend",
                      "title": f"{name} 日K", "klineCode": code}
    content = payload.setdefault("content", {})
    watch = [x for x in (content.get("financeWatchlist") or []) if isinstance(x, dict)]
    if not watch:
        owner = store.device_owner(mac)
        if owner:
            profile = store.get_user_profile(owner, "finance") or {}
            watch = [x for x in profile.get("watchlist", []) if isinstance(x, dict) and x.get("code")]
    if not any(str(x.get("code")) == code for x in watch):
        watch.insert(0, {"code": code, "name": name})
    content["financeWatchlist"] = watch[:8]
    store.save_content_config(mac, payload)
    store.set_pending_refresh(mac, True)
    store.audit(store.device_owner(mac) or "device", "device.finance.kline", mac,
                {"code": code, "name": name})
    return {"ok": True, "code": code, "name": name, "message": f"B 屏已切换为 {name}（{code}）日 K"}


# ——— 微信小程序：家长一键登录 ———
_WEXIN_JSCODE_URL = "https://api.weixin.qq.com/sns/jscode2session"


@router.post("/api/auth/wechat")
def wechat_login(body: dict[str, Any] = {}) -> dict[str, Any]:
    appid = (os.getenv("WECHAT_MINIAPP_APPID") or os.getenv("WECHAT_APPID") or "").strip()
    secret = (os.getenv("WECHAT_MINIAPP_SECRET") or os.getenv("WECHAT_APPSECRET") or "").strip()
    code = (body.get("code") or "").strip()
    if not code:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "缺少微信登录 code")
    if not appid or not secret:
        raise HTTPException(status.HTTP_503_SERVICE_UNAVAILABLE, "后端未配置小程序凭据（WECHAT_MINIAPP_APPID/SECRET）")
    try:
        url = _WEXIN_JSCODE_URL + "?" + urllib.parse.urlencode(
            {"appid": appid, "secret": secret, "js_code": code, "grant_type": "authorization_code"})
        with urllib.request.urlopen(url, timeout=10) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status.HTTP_502_BAD_GATEWAY, f"微信登录失败：{exc}")
    if data.get("errcode"):
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, f"微信登录失败：{data.get('errmsg','')}")
    openid = (data.get("openid") or "").strip()
    if not openid:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "微信未返回 openid")
    uid, username, is_new = store.get_or_create_wechat_user(openid)
    token = store.create_session_token(uid)
    return {"ok": True, "token": token, "user_id": uid, "username": username,
            "openid": openid, "is_new": is_new}


# ——— 微信小程序：老师授权码校验（免注册）———
@router.post("/api/auth/verify")
def verify_teacher_auth(body: dict[str, Any] = {}) -> dict[str, Any]:
    code = str(body.get("authCode") or body.get("auth_code") or "").strip().upper()
    if not code:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "缺少授权码")
    # 授权码可能跨设备，这里全局查
    with store.connect() as db:
        row = db.execute(
            "SELECT device_id, auth_name, permission, expire_at, frozen FROM teacher_codes WHERE auth_code=?",
            (code,)).fetchone()
    if not row:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "授权码无效")
    device_id, auth_name, perm, expire_at, frozen = row["device_id"], row["auth_name"], row["permission"], row["expire_at"], row["frozen"]
    if frozen:
        raise HTTPException(status.HTTP_403_FORBIDDEN, "该授权码已被冻结")
    if expire_at and expire_at < now_iso():
        raise HTTPException(status.HTTP_403_FORBIDDEN, "该授权码已过期")
    cfg = store.get_content_config(device_id) or {}
    student = (cfg.get("content", {}).get("child") or {}).get("name") or "学生"
    return {"ok": True, "authCode": code, "authName": auth_name or "老师", "deviceMac": device_id,
            "deviceSn": device_id, "studentName": student, "permission": perm or "only_note", "expireTime": expire_at}


# ——— 家长设备列表（小程序 /api/device/list）———
@router.get("/api/device/list")
def device_list(user_id: str = Depends(current_user)) -> dict[str, Any]:
    items = store.user_devices(user_id)
    out = []
    for d in items:
        out.append({
            "mac": d["device_id"], "sn": d["device_id"], "name": d.get("nickname") or "小悟看板",
            "nickname": d.get("nickname") or "", "is_online": online(d.get("last_seen_at")),
            "last_seen": d.get("last_seen_at") or "",
        })
    return {"devices": out}


# ——— 老师快捷推送：今日作业 / 老师便签 ———
_SYNC_COOLDOWN = 300
_sync_last: dict[str, float] = {}


def _verify_teacher(mac: str, code: str) -> dict[str, Any] | None:
    rec = store.get_teacher_code(mac, code)
    if not rec:
        return None
    if rec["is_frozen"]:
        return None
    if rec["expired"]:
        return None
    return rec


def _today() -> str:
    return datetime.now(UTC).strftime("%Y-%m-%d")


@router.post("/api/push/homework")
def push_homework(body: dict[str, Any] = {}) -> dict[str, Any]:
    mac = str(body.get("device_mac") or body.get("mac") or "").strip().upper()
    code = str(body.get("auth_code") or "").strip()
    auth = _verify_teacher(mac, code)
    if not auth:
        raise HTTPException(status.HTTP_403_FORBIDDEN, "授权码无效或已失效")
    if auth["permission"] != "note_and_homework":
        raise HTTPException(status.HTTP_403_FORBIDDEN, "该授权码无权推送作业")
    raw = (body.get("content") or "").strip()
    if not raw:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "作业内容为空")
    lines = [s.strip() for s in str(raw).replace("\r", "").split("\n") if s.strip()]
    is_append = int(body.get("is_append", 1)) != 0
    cfg = store.get_content_config(mac) or {"version": 1, "sceneKey": "study", "config": {}, "content": {}}
    content = cfg.setdefault("content", {})
    hw = content.get("homework") or []
    if not is_append:
        hw = [h for h in hw if (h.get("date") or "") != _today()]
    for t in lines:
        hw.append({"id": secrets.token_hex(4), "text": t, "done": False,
                   "sender": auth["auth_name"] or "老师", "sender_type": "teacher", "date": _today()})
    content["homework"] = hw
    cfg["content"] = content
    store.save_content_config(mac, cfg)
    store.set_pending_refresh(mac, True)
    store.save_push(mac, "homework", "teacher", auth["auth_name"] or "老师", raw, is_append, len(lines))
    return {"ok": True, "appended": len(lines), "mode": "append" if is_append else "overwrite"}


@router.post("/api/push/note")
def push_note(body: dict[str, Any] = {}) -> dict[str, Any]:
    mac = str(body.get("device_mac") or body.get("mac") or "").strip().upper()
    code = str(body.get("auth_code") or "").strip()
    auth = _verify_teacher(mac, code)
    if not auth:
        raise HTTPException(status.HTTP_403_FORBIDDEN, "授权码无效或已失效")
    raw = (body.get("content") or "").strip()
    if not raw:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "便签内容为空")
    persistent = int(body.get("persistent", 0)) != 0
    note = {"id": secrets.token_hex(4), "text": raw, "color": "yellow",
            "sender": auth["auth_name"] or "老师", "sender_type": "teacher", "persistent": persistent, "date": _today()}
    cfg = store.get_content_config(mac) or {"version": 1, "sceneKey": "study", "config": {}, "content": {}}
    content = cfg.setdefault("content", {})
    notes = content.get("notes") or []
    notes.append(note)
    content["notes"] = notes
    cfg["content"] = content
    store.save_content_config(mac, cfg)
    store.set_pending_refresh(mac, True)
    store.save_push(mac, "note", "teacher", auth["auth_name"] or "老师", raw, 1, 1)
    return {"ok": True, "persistent": persistent}


@router.get("/api/push/history")
def push_history(device_mac: str = "", auth_code: str = "", user_id: str | None = Depends(optional_user)) -> dict[str, Any]:
    mac = device_mac.strip().upper()
    if auth_code:
        if not _verify_teacher(mac, auth_code):
            raise HTTPException(status.HTTP_403_FORBIDDEN, "授权码无效或已失效")
    elif user_id and can_manage_device(user_id, mac):
        pass
    else:
        raise HTTPException(status.HTTP_403_FORBIDDEN, "无权查看该设备推送历史")
    items = store.get_push_history(mac)
    return {"items": items}


@router.delete("/api/push/history/{push_id}")
def delete_push(device_mac: str, push_id: int, user_id: str = Depends(current_user)) -> dict[str, Any]:
    mac = device_mac.strip().upper()
    if not can_manage_device(user_id, mac):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "无权操作该设备")
    ok = store.delete_push(mac, push_id)
    return {"ok": ok}


# ——— A 屏立即同步（5 分钟限流，保护墨水屏）———
@router.post("/api/device/{mac}/sync")
def sync_device(mac: str, user_id: str | None = Depends(optional_user),
                x_device_token: Annotated[str | None, Header()] = None) -> dict[str, Any]:
    mac = mac.strip().upper()
    if not (x_device_token and store.verify_device_token(mac, x_device_token)) and not (user_id and can_manage_device(user_id, mac)):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "无权同步该设备")
    now = time.time()
    last = _sync_last.get(mac, 0)
    if now - last < _SYNC_COOLDOWN:
        wait = int(_SYNC_COOLDOWN - (now - last))
        return {"ok": False, "cooldown": True, "message": f"墨水屏保护中，请 {wait} 秒后再同步"}
    _sync_last[mac] = now
    store.set_pending_refresh(mac, True)
    return {"ok": True, "cooldown": False, "message": "已推送，设备下次唤醒即刷新"}


# ——— 双屏快照预览地址（小程序 /api/device/snapshot）———
@router.get("/api/device/snapshot")
def device_snapshot(mac: str = Query(...), screen: str = Query(default="a"),
                   user_id: str | None = Depends(optional_user),
                   x_device_token: Annotated[str | None, Header()] = None) -> dict[str, Any]:
    mac = mac.strip().upper()
    if not (x_device_token and store.verify_device_token(mac, x_device_token)) and not (user_id and can_manage_device(user_id, mac)):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "无权查看该设备")
    cfg = store.get_content_config(mac) or {}
    screen_spec = cfg.get("screen") or {}
    colors = int(screen_spec.get("colors") or 4)
    scene_key = cfg.get("sceneKey") or "family"
    scene_cfg = (cfg.get("config") or {}).get(scene_key) or {}
    if screen.lower() == "a":
        persona = (scene_cfg.get("a") or {}).get("mode") or "STOIC"
        url = f"/api/preview?mac={mac}&persona={persona}&colors=2&bpp=1"
    else:
        url = f"/api/preview?mac={mac}&persona=STOIC&colors={colors}&bpp=2"
    return {"ok": True, "url": url, "mac": mac, "screen": screen.lower()}
