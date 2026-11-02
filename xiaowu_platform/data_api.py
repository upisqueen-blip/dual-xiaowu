"""轨2「数据接口」：ISV 提供 HTTP 端点，返回平台约定的 e-ink 卡片 JSON。

背景：设备固件不渲染 HTML，屏幕内容由后端 PIL 出图。因此无网页的桌面 / 服务端
应用（如 codex-pulse）要上屏，必须提供*数据*而非*页面*——平台按契约把数据画成
墨水屏卡片。这就是本模块承载的「数据接口」提交轨。

契约 v1（GET，返回 application/json）::

    {
      "version": 1,
      "title": "API 额度",                 // 必填，<= 24 字
      "subtitle": "codex pulse",          // 选填，<= 32 字
      "items": [                          // 必填，1 ~ 6 项
        {"label": "剩余额度", "value": "$42.10", "emphasis": "red"},
        {"label": "到期倒计时", "value": "6 天"},
        {"label": "本月用量", "value": "68%", "progress": 68}
      ],
      "footer": "更新于 17:08",            // 选填，<= 40 字
      "qr": "https://example.com/app",     // 选填，http(s) 入口二维码
      "refresh_minutes": 15                // 选填，1 ~ 1440
    }

平台请求端点时会附带 ``?screen=A`` 或 ``?screen=B``，便于 ISV 针对双屏返回不同内容；
端点可忽略该参数，两屏即显示相同内容。若端点需要鉴权，平台会带
``Authorization: Bearer <token>``（token 由开发者在提交时填写）。

校验结论分 pass / warn / fail：仅 fail 会阻止提交审核；warn 为优化建议。
"""

from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

SCHEMA_VERSION = 1
MAX_ITEMS = 6
MAX_BYTES = 32 * 1024
DEFAULT_TIMEOUT = 6
LIMITS = {"title": 24, "subtitle": 32, "footer": 40, "label": 12, "value": 20}
VALID_EMPHASIS = {"none", "red", "yellow"}


class DataApiError(Exception):
    """抓取或解析失败，message 直接回显给开发者。"""


def _clean(value: Any) -> str:
    """去空白、压空格、转字符串（None → 空串）。"""
    if value is None:
        return ""
    text = " ".join(str(value).split())
    return text


def _as_int(value: Any, default: int) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def normalize_card(payload: dict[str, Any]) -> dict[str, Any]:
    """把（可能不合规的）payload 规整成渲染器可安全消费的结构。"""
    payload = payload if isinstance(payload, dict) else {}
    items: list[dict[str, Any]] = []
    raw_items = payload.get("items")
    if isinstance(raw_items, list):
        for raw in raw_items[:MAX_ITEMS]:
            if not isinstance(raw, dict):
                continue
            emphasis = _clean(raw.get("emphasis")).lower()
            if emphasis not in VALID_EMPHASIS:
                emphasis = "none"
            item: dict[str, Any] = {
                "label": _clean(raw.get("label"))[: LIMITS["label"]],
                "value": _clean(raw.get("value"))[: LIMITS["value"]],
                "emphasis": emphasis,
            }
            progress = raw.get("progress")
            if progress is not None:
                item["progress"] = max(0, min(100, _as_int(progress, 0)))
            items.append(item)
    refresh = _as_int(payload.get("refresh_minutes"), 15)
    return {
        "version": _as_int(payload.get("version"), SCHEMA_VERSION),
        "title": _clean(payload.get("title"))[: LIMITS["title"]],
        "subtitle": _clean(payload.get("subtitle"))[: LIMITS["subtitle"]],
        "items": items,
        "footer": _clean(payload.get("footer"))[: LIMITS["footer"]],
        "qr": _clean(payload.get("qr")),
        "refresh_minutes": max(1, min(1440, refresh)),
    }


def validate_card(payload: Any) -> dict[str, Any]:
    """校验 payload 是否符合契约 v1，返回 checks / verdict / normalized。"""
    checks: list[dict[str, Any]] = []

    def add(check_id: str, label: str, status: str, detail: str) -> None:
        checks.append({"id": check_id, "label": label, "status": status, "detail": detail})

    if not isinstance(payload, dict):
        add("json_object", "返回 JSON 对象", "fail", "端点必须返回 JSON 对象（{...}）")
        return {
            "passed": False,
            "verdict": "fail",
            "checks": checks,
            "summary": {"pass": 0, "warn": 0, "fail": 1},
            "normalized": normalize_card({}),
            "schema_version": SCHEMA_VERSION,
        }

    version = _as_int(payload.get("version"), 0)
    if version == SCHEMA_VERSION:
        add("schema_version", "契约版本", "pass", f"version = {SCHEMA_VERSION}")
    elif version == 0:
        add("schema_version", "契约版本", "warn", "未声明 version，按 v1 处理；建议在响应中加 \"version\": 1")
    else:
        add("schema_version", "契约版本", "warn", f"version = {version}，平台当前按 v1 解析")

    title = _clean(payload.get("title"))
    if not title:
        add("title", "标题", "fail", "缺少 title：请在响应中提供卡片标题（<=24 字）")
    elif len(title) > LIMITS["title"]:
        add("title", "标题", "warn", f"title 超过 {LIMITS['title']} 字，超出部分会被截断")
    else:
        add("title", "标题", "pass", f"title = {title}")

    raw_items = payload.get("items")
    if not isinstance(raw_items, list):
        add("items", "数据项", "fail", "缺少 items 数组：请提供 1~6 个 {label, value} 项")
        raw_items = []
    elif not raw_items:
        add("items", "数据项", "fail", "items 为空：至少提供 1 项数据")
    elif len(raw_items) > MAX_ITEMS:
        add("items", "数据项", "warn", f"items 有 {len(raw_items)} 项，超过 {MAX_ITEMS} 项，超出部分不显示")
    else:
        add("items", "数据项", "pass", f"共 {len(raw_items)} 项")

    bad_items = 0
    long_items = 0
    for idx, raw in enumerate(raw_items[:MAX_ITEMS]):
        if not isinstance(raw, dict):
            bad_items += 1
            continue
        label = _clean(raw.get("label"))
        value = _clean(raw.get("value"))
        if not label and not value:
            bad_items += 1
            continue
        if len(label) > LIMITS["label"] or len(value) > LIMITS["value"]:
            long_items += 1
        emphasis = _clean(raw.get("emphasis")).lower()
        if emphasis and emphasis not in VALID_EMPHASIS:
            long_items += 0  # emphasis 非法已在 normalize 兜底，不单独失败
    if bad_items:
        add("items_shape", "数据项结构", "fail", f"有 {bad_items} 项缺少 label/value，请检查 items 结构")
    elif long_items:
        add("items_shape", "数据项结构", "warn",
            f"有 {long_items} 项文本过长（label<={LIMITS['label']}、value<={LIMITS['value']}），会被截断")
    elif raw_items:
        add("items_shape", "数据项结构", "pass", "每项均含 label/value")

    if "progress" in payload:
        add("progress", "进度字段", "warn", "items 内的 progress 请放在单项里（0~100），顶层 progress 会被忽略")

    qr = _clean(payload.get("qr"))
    if qr and not qr.lower().startswith(("http://", "https://")):
        add("qr", "入口二维码", "warn", "qr 需为 http(s) 绝对地址，否则不生成二维码")
    elif qr:
        add("qr", "入口二维码", "pass", "已提供入口地址")

    refresh = payload.get("refresh_minutes")
    if refresh is not None:
        rv = _as_int(refresh, 0)
        if rv < 1 or rv > 1440:
            add("refresh", "刷新频率", "warn", "refresh_minutes 需在 1~1440 之间，已按边界处理")

    failed = [c for c in checks if c["status"] == "fail"]
    warned = [c for c in checks if c["status"] == "warn"]
    return {
        "passed": not failed,
        "verdict": "fail" if failed else ("warn" if warned else "pass"),
        "checks": checks,
        "summary": {
            "pass": sum(1 for c in checks if c["status"] == "pass"),
            "warn": len(warned),
            "fail": len(failed),
        },
        "normalized": normalize_card(payload),
        "schema_version": SCHEMA_VERSION,
    }


def _build_url(endpoint: str, screen: str) -> str:
    parsed = urllib.parse.urlparse(endpoint)
    if parsed.scheme not in ("http", "https"):
        raise DataApiError("端点必须是 http(s) 地址")
    if not parsed.netloc:
        raise DataApiError("端点地址不合法")
    query = urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)
    query = [kv for kv in query if kv[0] != "screen"]
    query.append(("screen", screen))
    return urllib.parse.urlunparse(parsed._replace(query=urllib.parse.urlencode(query)))


def fetch_card(endpoint: str, token: str = "", screen: str = "A",
               timeout: int = DEFAULT_TIMEOUT) -> dict[str, Any]:
    """抓取 ISV 数据端点，返回解析后的 JSON（未校验）。失败抛 DataApiError。"""
    url = _build_url(endpoint, "B" if str(screen).upper() == "B" else "A")
    headers = {"Accept": "application/json", "User-Agent": "xiaowu-data-api/1.0"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, headers=headers, method="GET")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read(MAX_BYTES + 1)
    except urllib.error.HTTPError as exc:
        raise DataApiError(f"端点返回 HTTP {exc.code}") from exc
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise DataApiError(f"无法访问端点：{exc}") from exc
    if len(raw) > MAX_BYTES:
        raise DataApiError(f"响应超过 {MAX_BYTES // 1024} KB 上限")
    try:
        payload = json.loads(raw.decode("utf-8", "ignore"))
    except (ValueError, UnicodeDecodeError) as exc:
        raise DataApiError(f"响应不是合法 JSON：{exc}") from exc
    return payload
