"""理财场景：自选行情 + K 线渲染（原型实现）。

数据源：腾讯公开行情接口（主源，qt.gtimg.cn / web.ifzq.gtimg.cn），
东方财富公开接口（备源，主源不可用时自动切换）。
均为非官方授权接口，仅供原型/内测。商用化阶段须替换为持牌行情数据商
（同花顺 iFinD B 端 / 融聚汇 / Tushare Pro 等），并完成交易所行情信息分发授权。

能力：
- resolve_secid：证券代码 → 市场 secid（沪 6/5/9→1.，深 0/3 与北交 8/4→0.）
- fetch_quotes：批量实时快照（名称 / 现价 / 涨跌幅 / 涨跌额），60s 内存缓存
- fetch_kline：日 K（前复权，不足时回落不复权）
- render_finance_list：A 屏 1-bit 自选列表 / 市场摘要
- render_kline_image：B 屏日 K 蜡烛图（四色调色板：红涨、黑跌；黑白屏空心/实心）
"""
from __future__ import annotations

import json
import logging
import re
import ssl
import time
import urllib.parse
import urllib.request
import zlib
from datetime import datetime
from typing import Any, Optional

from PIL import Image, ImageDraw

logger = logging.getLogger(__name__)

_QQ_QUOTE_URL = "https://qt.gtimg.cn/q="
_QQ_KLINE_URL = "https://web.ifzq.gtimg.cn/appstock/app/fqkline/get"
_EM_QUOTE_URL = "https://push2.eastmoney.com/api/qt/ulist.np/get"
_EM_KLINE_URL = "https://push2his.eastmoney.com/api/qt/stock/kline/get"
_HEADERS = {
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
    "Referer": "https://gu.qq.com/",
    "Accept": "application/json, text/plain, */*",
}
_TIMEOUT = 4.0
_RETRIES = 2
_CACHE_TTL = 60.0
_MAX_CACHE = 256
_cache: dict[str, tuple[float, Any]] = {}
# 失败缓存：上游不可达（如本机网络封锁东财）时，别在每次渲染都重试刷错。
_fail_cache: dict[str, tuple[float, Exception]] = {}
_SECTOR_SINA_CACHE: Optional[tuple[float, list[dict]]] = None
# 板块结果缓存：避免每屏渲染都去打一次注定失败的东财概念接口。
_sectors_cache: Optional[tuple[float, dict[str, list[dict]]]] = None

# 直连 opener：绕过 http_proxy/https_proxy 等环境代理，
# 避免本机代理（Clash 等）劫持或断连公开行情接口。
_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

# 东财 push2 主站在部分网络下偶发 RemoteDisconnected（服务端无响应直接断连）。
# 依次尝试下列 CDN 镜像可显著提升命中率；配合 Connection: close 避免复用已失效的长连接。
_EM_MIRRORS = [
    "push2.eastmoney.com",
    "push2delay.eastmoney.com",
    "push2his.eastmoney.com",
    "1.push2.eastmoney.com",
    "2.push2.eastmoney.com",
    "3.push2.eastmoney.com",
    "4.push2.eastmoney.com",
    "5.push2.eastmoney.com",
    "7.push2.eastmoney.com",
    "8.push2.eastmoney.com",
    "9.push2.eastmoney.com",
    "10.push2.eastmoney.com",
    "11.push2.eastmoney.com",
    "12.push2.eastmoney.com",
    "13.push2.eastmoney.com",
]

# 市场摘要模式默认展示的三大指数
INDEX_SUMMARY_CODES = ["sh000001", "sz399001", "sz399006"]


def resolve_secid(code: str) -> str:
    """600519 / sh600519 / 1.600519 → 东财 secid。"""
    raw = str(code).strip().lower()
    if raw[:2] in ("1.", "0."):
        raw = raw[2:]
    m = re.match(r"^(sh|sz|bj)?(\d{6})$", raw)
    if not m:
        raise ValueError(f"无效的证券代码：{code}")
    prefix, num = m.group(1), m.group(2)
    if prefix == "sh":
        market = 1
    elif prefix in ("sz", "bj"):
        market = 0
    else:
        market = 1 if num[0] in ("6", "5", "9") else 0
    return f"{market}.{num}"


def _get_json(url: str) -> dict[str, Any]:
    now = time.time()
    hit = _cache.get(url)
    if hit and now - hit[0] < _CACHE_TTL:
        return hit[1]
    # 近期已失败过：直接抛出缓存的异常，避免每次渲染都重新重试刷错日志。
    fail = _fail_cache.get(url)
    if fail and now - fail[0] < _CACHE_TTL:
        raise fail[1]

    parsed = urllib.parse.urlparse(url)
    is_em = parsed.netloc.endswith("eastmoney.com")
    # 东财：跨镜像 host 重试；非东财（腾讯）：仅原 host。
    hosts = _EM_MIRRORS[:4] if is_em else [parsed.netloc]
    # 东财镜像响应快、单次即可，减少每镜像内部重试；非东财沿用原重试次数。
    inner = 1 if is_em else _RETRIES

    data: dict[str, Any] | None = None
    last_exc: Exception | None = None
    for host in hosts:
        cand = url if host == parsed.netloc else url.replace(parsed.netloc, host, 1)
        req = urllib.request.Request(cand, headers={**_HEADERS, "Connection": "close"})
        for attempt in range(inner + 1):
            try:
                with _opener.open(req, timeout=_TIMEOUT) as resp:
                    data = json.loads(resp.read().decode("utf-8"))
                break
            except Exception as exc:  # noqa: BLE001
                last_exc = exc
                if attempt < inner:
                    time.sleep(0.3 * (attempt + 1))
        if data is not None:
            break
    if data is None:
        exc = last_exc if last_exc else RuntimeError("request failed")
        _fail_cache[url] = (now, exc)
        raise exc
    _fail_cache.pop(url, None)
    if len(_cache) >= _MAX_CACHE:
        oldest = min(_cache, key=lambda k: _cache[k][0])
        _cache.pop(oldest, None)
    _cache[url] = (now, data)
    return data


def _num(value: Any) -> Optional[float]:
    return float(value) if isinstance(value, (int, float)) else None


def _symbol(code: str) -> str:
    """600519 → sh600519（腾讯符号）。北交所代码腾讯接口不支持，走东财备源。"""
    secid = resolve_secid(code)
    return ("sh" if secid[0] == "1" else "sz") + secid[2:]


def fetch_quotes(codes: list[str]) -> list[dict[str, Any]]:
    """批量实时快照。停牌/无效代码的数值字段为 None。"""
    if not codes:
        return []
    try:
        return _quotes_tencent(codes)
    except Exception as exc:  # noqa: BLE001
        logger.warning("[FINANCE] tencent quote failed, fallback to eastmoney: %s", exc)
    try:
        return _quotes_eastmoney(codes)
    except Exception as exc:  # noqa: BLE001
        logger.warning("[FINANCE] eastmoney quote failed: %s", exc)
        return []


def _quotes_tencent(codes: list[str]) -> list[dict[str, Any]]:
    url = _QQ_QUOTE_URL + ",".join(_symbol(c) for c in codes)
    req = urllib.request.Request(url, headers=_HEADERS)
    with _opener.open(req, timeout=_TIMEOUT) as resp:
        text = resp.read().decode("gbk", errors="replace")
    by_symbol: dict[str, dict[str, Any]] = {}
    for m in re.finditer(r'v_(sh|sz)(\d{6})="([^"]*)"', text):
        parts = m.group(3).split("~")
        if len(parts) < 33:
            continue
        by_symbol[m.group(1) + m.group(2)] = {
            "code": parts[2],
            "name": parts[1],
            "price": _num(float(parts[3])) if parts[3] else None,
            "chg": _num(float(parts[31])) if parts[31] else None,
            "pct": _num(float(parts[32])) if parts[32] else None,
            "market": 1 if m.group(1) == "sh" else 0,
        }
    return [by_symbol.get(_symbol(c), {"code": c[-6:], "name": "", "price": None, "pct": None, "chg": None, "market": 0})
            for c in codes]


def _quotes_eastmoney(codes: list[str]) -> list[dict[str, Any]]:
    secids = [resolve_secid(c) for c in codes]
    url = f"{_EM_QUOTE_URL}?fltt=2&invt=2&fields=f2,f3,f4,f12,f13,f14&secids={','.join(secids)}"
    data = _get_json(url)
    diff = ((data or {}).get("data") or {}).get("diff") or []
    if isinstance(diff, dict):
        diff = list(diff.values())
    return [
        {
            "code": str(row.get("f12") or ""),
            "name": str(row.get("f14") or ""),
            "price": _num(row.get("f2")),
            "pct": _num(row.get("f3")),
            "chg": _num(row.get("f4")),
            "market": int(row.get("f13") or 0),
        }
        for row in diff
    ]


def fetch_kline(code: str, days: int = 60) -> dict[str, Any]:
    """日 K（前复权）。返回 {code, name, items: [{date, open, close, high, low}]}。"""
    try:
        return _kline_tencent(code, days)
    except Exception as exc:  # noqa: BLE001
        logger.warning("[FINANCE] tencent kline failed, fallback to eastmoney: %s", exc)
    return _kline_eastmoney(code, days)


def _kline_tencent(code: str, days: int) -> dict[str, Any]:
    sym = _symbol(code)
    days = max(10, min(int(days), 250))
    url = f"{_QQ_KLINE_URL}?param={sym},day,,,{days},qfq"
    data = _get_json(url).get("data") or {}
    node = data.get(sym) or {}
    rows = node.get("qfqday") or node.get("day") or []
    items = []
    for row in rows:
        if len(row) < 5:
            continue
        try:
            items.append({
                "date": str(row[0]),
                "open": float(row[1]),
                "close": float(row[2]),
                "high": float(row[3]),
                "low": float(row[4]),
            })
        except (TypeError, ValueError):
            continue
    qt = ((node.get("qt") or {}).get(sym) or [])
    name = qt[1] if len(qt) > 1 else ""
    return {"code": code[-6:], "name": str(name), "items": items}


def _kline_eastmoney(code: str, days: int) -> dict[str, Any]:
    secid = resolve_secid(code)
    days = max(10, min(int(days), 250))
    url = (f"{_EM_KLINE_URL}?secid={secid}&fields1=f1,f2,f3,f4,f5,f6"
           f"&fields2=f51,f52,f53,f54,f55,f56&klt=101&fqt=1&end=20500101&lmt={days}")
    try:
        data = (_get_json(url) or {}).get("data") or {}
    except Exception as exc:  # noqa: BLE001
        logger.warning("[FINANCE] eastmoney kline fetch failed: %s", exc)
        data = {}
    items: list[dict[str, Any]] = []
    for line in data.get("klines") or []:
        parts = str(line).split(",")
        if len(parts) < 5:
            continue
        try:
            items.append({
                "date": parts[0],
                "open": float(parts[1]),
                "close": float(parts[2]),
                "high": float(parts[3]),
                "low": float(parts[4]),
            })
        except ValueError:
            continue
    return {"code": str(data.get("code") or code), "name": str(data.get("name") or ""), "items": items}


def _cjk(size: int):
    from .render import _load_cjk
    return _load_cjk(size)


def _load_cjk_bold(size: int):
    from .render import _load_cjk
    return _load_cjk(size, bold=True)


def _palette(w: int, h: int, colors: int) -> Image.Image:
    from .render import _palette_p_image
    return _palette_p_image(w, h, colors)


def _fmt(value: Optional[float], digits: int = 2, na: str = "--") -> str:
    return f"{value:.{digits}f}" if isinstance(value, float) else na


def _pct_text(pct: Optional[float]) -> str:
    if not isinstance(pct, float):
        return "--"
    arrow = "▲" if pct >= 0 else "▼"
    return f"{arrow}{abs(pct):.2f}%"


# ── A 屏：自选列表 / 市场摘要（1-bit 黑白） ─────────────────────
def _is_up(q: dict[str, Any]) -> bool:
    """涨跌判定（腾讯字段是字符串，容错转 float）。A 股习惯：涨=红、跌=黑。"""
    try:
        return float(str(q.get("pct") or "0").strip() or 0) > 0
    except (TypeError, ValueError):
        return False


def render_finance_list(quotes: list[dict[str, Any]], w: int, h: int,
                        title: str = "自选行情", footer: str = "",
                        colors: int = 2) -> Image.Image:
    colored = colors >= 3
    img = _palette(w, h, colors) if colored else Image.new("1", (w, h), 1)
    draw = ImageDraw.Draw(img)
    s = w / 400.0
    pad = int(12 * s)
    f_title = _cjk(max(14, int(17 * s)))
    f_meta = _cjk(max(10, int(11 * s)))
    f_name = _cjk(max(11, int(13 * s)))
    f_price = _cjk(max(11, int(13 * s)))

    today = datetime.now().strftime("%m-%d %H:%M")
    draw.text((pad, int(8 * s)), title, fill=0, font=f_title)
    tw = draw.textlength(today, font=f_meta)
    draw.text((w - pad - tw, int(12 * s)), today, fill=0, font=f_meta)
    draw.line([(pad, int(32 * s)), (w - pad, int(32 * s))], fill=0, width=max(1, int(s)))

    rows = quotes[:8]
    top, bottom = int(38 * s), h - int(18 * s)
    if not rows:
        draw.text((pad, (top + bottom) // 2 - int(8 * s)), "暂无自选，请在后台「理财自选」添加",
                  fill=0, font=f_name)
        return img
    row_h = (bottom - top) // len(rows)
    # 行数少时不许行高无限撑满（如三大指数只有 3 行），保持紧凑列表观感
    row_h = min(row_h, int(34 * s))
    for i, q in enumerate(rows):
        y = top + i * row_h
        name = str(q.get("name") or q.get("code") or "--")
        draw.text((pad, y + row_h // 2 - int(9 * s)), name, fill=0, font=f_name)
        price = _fmt(q.get("price"))
        pw = draw.textlength(price, font=f_price)
        draw.text((int(w * 0.62) - pw, y + row_h // 2 - int(9 * s)), price, fill=0, font=f_price)
        pct = _pct_text(q.get("pct"))
        cw = draw.textlength(pct, font=f_price)
        # 三色/四色面板：涨用红色（索引 3），跌保持黑色。黑白面板恒黑。
        draw.text((w - pad - cw, y + row_h // 2 - int(9 * s)), pct,
                  fill=3 if (colored and _is_up(q)) else 0, font=f_price)
        if i:
            draw.line([(pad, y), (w - pad, y)], fill=0, width=1)
    foot = footer or "行情快照 · 腾讯公开接口（原型）"
    fw = draw.textlength(foot, font=f_meta)
    draw.text((w - pad - fw, h - int(14 * s)), foot, fill=0, font=f_meta)
    return img


# ── B 屏：日 K 蜡烛图（调色板 / 黑白自适应） ───────────────────
def render_kline_image(kline: dict[str, Any], w: int, h: int, colors: int) -> Image.Image:
    items = kline.get("items") or []
    name = kline.get("name") or kline.get("code") or "--"
    colored = colors >= 3
    img = _palette(w, h, colors) if colored else Image.new("1", (w, h), 1)
    draw = ImageDraw.Draw(img)
    s = w / 400.0
    pad = int(10 * s)
    f_title = _load_cjk_bold(max(13, int(15 * s)))
    f_meta = _cjk(max(9, int(10 * s)))

    if len(items) < 2:
        draw.text((pad, int(40 * s)), "K 线数据获取失败，稍后自动重试", fill=0, font=f_title)
        return img

    # 标题：名称 + 最新价 + 最新涨跌幅
    last, prev = items[-1], items[-2]
    pct = (last["close"] - prev["close"]) / prev["close"] * 100 if prev["close"] else 0.0
    draw.text((pad, int(8 * s)), f"{name}  {last['close']:.2f}", fill=0, font=f_title)
    # 涨跌幅单独上色：三色屏红涨、黑跌（A 股习惯）；黑白屏保持黑字
    _head = f"{name}  {last['close']:.2f}"
    _hx = pad + int(draw.textlength(_head, font=f_title)) + int(9 * s)
    draw.text((_hx, int(8 * s)), _pct_text(pct),
              fill=(3 if (colored and pct >= 0) else 0), font=f_title)
    days_label = f"日K · {items[0]['date'][5:]} ~ {last['date'][5:]}"
    dw = draw.textlength(days_label, font=f_meta)
    draw.text((w - pad - dw, int(12 * s)), days_label, fill=0, font=f_meta)

    left = int(44 * s)          # 左侧留给价格刻度
    right, top, bottom = w - pad, int(34 * s), h - int(18 * s)
    lo = min(x["low"] for x in items)
    hi = max(x["high"] for x in items)
    if hi <= lo:
        hi = lo + 1.0
    span = hi - lo
    lo -= span * 0.05
    hi += span * 0.05

    def py(price: float) -> int:
        return int(bottom - (price - lo) / (hi - lo) * (bottom - top))

    # 横向网格 + 价格刻度（4 档）
    f_axis = _cjk(max(8, int(9 * s)))
    for i in range(4):
        price = lo + (hi - lo) * (i + 0.5) / 4
        y = py(price)
        draw.line([(left, y), (right, y)], fill=0, width=1)
        label = f"{price:.2f}"
        draw.text((pad, y - int(5 * s)), label, fill=0, font=f_axis)

    # 蜡烛
    chart_w = right - left
    slot = chart_w / len(items)
    body_w = max(1, int(slot * 0.6))
    red = 3  # 调色板索引：3=红
    for i, k in enumerate(items):
        cx = left + int(i * slot + slot / 2)
        up = k["close"] >= k["open"]
        wick = red if (colored and up) else 0
        draw.line([(cx, py(k["high"])), (cx, py(k["low"]))], fill=wick, width=1)
        y_top, y_bot = py(max(k["open"], k["close"])), py(min(k["open"], k["close"]))
        if y_bot - y_top < 1:
            y_bot = y_top + 1
        bx = cx - body_w // 2
        if up and colored:
            draw.rectangle([bx, y_top, bx + body_w, y_bot], fill=red)  # 红涨（A股惯例）
        elif up:
            draw.rectangle([bx, y_top, bx + body_w, y_bot], outline=0, width=1)  # 黑白屏：空心
        else:
            draw.rectangle([bx, y_top, bx + body_w, y_bot], fill=0)  # 跌：实心黑

    return img


# ── 渲染入口（由 render_dual_screen 的理财分支调用） ───────────
def render_a_screen(a_cfg: dict[str, Any], dual_config: Optional[dict], w: int, h: int,
                    mac: str = "", colors: int = 2) -> Image.Image:
    """A 屏理财模式：finance-market 大盘全景 / finance-summary 市场摘要 / finance-watch 自选行情。

    colors 由后台「A 屏面板色彩」决定：2=黑白，3/4 时涨跌带红色强调。
    """
    mode = str(a_cfg.get("mode") or "finance-watch")
    if mode == "finance-market":
        groups = fetch_market_indices()
        return render_market_board(groups, w, h, colors=colors)
    watchlist = _effective_watchlist(mac, dual_config)
    if mode == "finance-summary":
        quotes = fetch_quotes(INDEX_SUMMARY_CODES)
        return render_finance_list(quotes, w, h, title="市场摘要",
                                   footer="指数快照 · 腾讯公开接口（原型）", colors=colors)
    quotes = fetch_quotes(watchlist) if watchlist else []
    return render_finance_list(quotes, w, h, title="自选行情", colors=colors)


def render_b_screen(b_cfg: dict[str, Any], dual_config: Optional[dict], w: int, h: int, colors: int,
                    mac: str = "") -> Image.Image:
    """B 屏理财模式：finance-sectors 板块动向 / finance-trend|finance-kline → A 屏自选首只日 K。"""
    mode = str(b_cfg.get("mode") or "finance-trend")
    if mode == "finance-sectors":
        sectors = fetch_sectors()
        commodities = fetch_commodities()
        return render_sectors(sectors, commodities, w, h, colors)
    watchlist = _effective_watchlist(mac, dual_config)
    code = str(b_cfg.get("klineCode") or "").strip()
    if not code:
        code = watchlist[0] if watchlist else "sh000001"
    kline = fetch_kline(code, days=60)
    if not kline["name"]:
        kline["name"] = kline["code"]
    return render_kline_image(kline, w, h, colors)


def _effective_watchlist(mac: str, dual_config: Optional[dict]) -> list[str]:
    """设备配置的 content.financeWatchlist 优先；为空时回落到绑定账号的理财档案自选。

    保证「A 屏选的股票 → B 屏 K 线」不需要手动「应用到设备」也成立。
    """
    codes = _device_watchlist(dual_config)
    if codes:
        return codes
    if not mac:
        return []
    try:
        from .store import Store
        from .settings import settings
        store = Store(settings.database_path)
        owner = store.device_owner(mac.strip().upper())
        if not owner:
            return []
        profile = store.get_user_profile(owner, "finance") or {}
        out = []
        for item in profile.get("watchlist", []):
            code = item.get("code") if isinstance(item, dict) else str(item)
            if code:
                out.append(str(code))
        return out[:8]
    except Exception:  # noqa: BLE001 — 渲染路径不允许因自选回落失败而出错
        return []


def _device_watchlist(dual_config: Optional[dict]) -> list[str]:
    """从设备 dual_config 读取自选代码（content.financeWatchlist）。"""
    if not dual_config:
        return []
    raw = (dual_config.get("content") or {}).get("financeWatchlist") or []
    out = []
    for item in raw:
        code = item.get("code") if isinstance(item, dict) else str(item)
        if code:
            out.append(str(code))
    return out[:8]


# ——— 推荐股（动量筛选原型）与资金管理（持仓视图合成）———
# 纯计算函数，行情由调用方注入，便于单测；两端点见 console_api.py。
# 合规边界：仅基于公开行情的规则化排序/统计，输出必须携带免责声明。

# 推荐候选池：流动性较好的宽基/行业代表（原型，商用需换持牌数据源+策略引擎）
RECO_POOL = [
    "600519", "601318", "600036", "000858", "600900",
    "000333", "600276", "601899", "300750", "002594",
    "601012", "600030",
]


def rank_recommendations(quotes: list[dict[str, Any]], top: int = 5) -> list[dict[str, Any]]:
    """按当日涨跌幅（动量）排序取前 N，附规则化理由标签。"""
    valid = [q for q in quotes if q.get("pct") is not None and q.get("name")]
    ranked = sorted(valid, key=lambda q: q["pct"], reverse=True)[: max(1, top)]
    items = []
    for q in ranked:
        pct = q["pct"]
        if pct >= 5:
            reason = "今日强势领涨"
        elif pct >= 2:
            reason = "日内动量走强"
        elif pct >= 0:
            reason = "温和上行"
        elif pct > -2:
            reason = "小幅回调"
        else:
            reason = "明显回调"
        items.append({
            "code": q.get("code"),
            "name": q.get("name"),
            "price": q.get("price"),
            "pct": pct,
            "reason": reason,
        })
    return items


RECO_DISCLAIMER = "推荐结果为公开行情的规则化排序，仅供参考，不构成投资建议"


def build_portfolio_view(positions: list[dict[str, Any]], quotes: list[dict[str, Any]],
                         cash: float = 0.0) -> dict[str, Any]:
    """持仓 × 实时行情 → 逐只盈亏/权重/调仓参考 + 汇总。

    positions: [{code, name?, shares>0, cost>0}]；quotes 与 fetch_quotes 输出同构。
    中国市场惯例：涨为正（红）、跌为负（绿），前端按正负着色。
    """
    by_code = {q.get("code"): q for q in quotes}
    rows = []
    total_value = 0.0
    total_cost = 0.0
    for p in positions:
        try:
            shares = float(p.get("shares") or 0)
            cost = float(p.get("cost") or 0)
        except (TypeError, ValueError):
            shares, cost = 0.0, 0.0
        if shares <= 0 or cost <= 0:
            continue
        q = by_code.get(str(p.get("code"))) or {}
        price = q.get("price")
        name = q.get("name") or p.get("name") or str(p.get("code"))
        value = round(shares * price, 2) if price is not None else None
        pnl = round((price - cost) * shares, 2) if price is not None else None
        pnl_pct = round((price / cost - 1) * 100, 2) if price is not None else None
        rows.append({
            "code": p.get("code"), "name": name, "shares": shares, "cost": cost,
            "price": price, "value": value, "pnl": pnl, "pnl_pct": pnl_pct,
            "weight": None,  # 第二遍填充
            "advice": "停牌或无行情，维持观察" if price is None else "",
        })
        if value is not None:
            total_value += value
        total_cost += cost * shares

    n = len(rows)
    for r in rows:
        weight = round(r["value"] / total_value * 100, 1) if (r["value"] is not None and total_value > 0) else None
        r["weight"] = weight
        if r["advice"]:
            continue
        pnl_pct, dev = r["pnl_pct"], (weight - 100.0 / n) if (weight is not None and n) else 0.0
        if pnl_pct >= 15:
            r["advice"] = "累计涨幅较大，可关注止盈节奏"
        elif pnl_pct <= -15:
            r["advice"] = "回撤较大，关注仓位与风险"
        elif dev > 10:
            r["advice"] = f"仓位高于平均 {dev:.0f} 个百分点，可考虑减持均衡"
        elif dev < -10:
            r["advice"] = "仓位低于平均，可考虑逢低增持"
        else:
            r["advice"] = "持有观察"

    total_pnl = round(total_value - total_cost, 2) if total_value else 0.0
    hot = max(rows, key=lambda r: r["weight"] or 0) if rows else None
    summary = {
        "market_value": round(total_value, 2),
        "cost_value": round(total_cost, 2),
        "pnl": total_pnl,
        "pnl_pct": round(total_pnl / total_cost * 100, 2) if total_cost else None,
        "cash": round(cash, 2),
        "count": n,
    }
    notes = []
    if hot and hot["weight"] is not None and hot["weight"] > 40:
        notes.append(f"{hot['name']} 占比 {hot['weight']:.0f}%，仓位较集中")
    if cash > 0 and total_value > 0 and cash / (cash + total_value) > 0.5:
        notes.append("现金占比过半，资金利用率偏低")
    return {"positions": rows, "summary": summary, "notes": notes,
            "disclaimer": "调仓参考为规则化提示，不构成投资建议"}


def search_security(keyword: str, count: int = 5) -> list[dict[str, Any]]:
    """名称/拼音/代码 → 候选证券（东方财富公开 suggest 接口，仅 A 股）。

    返回 [{code(6位), name, market(0深1沪)}]，供持仓录入时自动反查代码。
    """
    kw = (keyword or "").strip()
    if not kw:
        return []
    if re.fullmatch(r"\d{6}", kw):
        return [{"code": kw, "name": "", "market": 1 if kw.startswith(("6", "9", "5")) else 0}]
    url = (
        "https://searchapi.eastmoney.com/api/suggest/get"
        f"?input={urllib.parse.quote(kw)}&type=14&token=D43BF722C8E33BDC906FB84D85E326E8"
        f"&count={max(1, min(count, 10))}"
    )
    headers = {"User-Agent": "Mozilla/5.0", "Referer": "https://www.eastmoney.com/"}
    req = urllib.request.Request(url, headers=headers)
    try:
        with _opener.open(req, timeout=8) as resp:
            data = json.load(resp)
    except Exception:
        logger.warning("security search failed: %s", kw, exc_info=True)
        return []
    items = ((data.get("QuotationCodeTable") or {}).get("Data")) or []
    out = []
    for it in items:
        code = str(it.get("Code") or "")
        name = str(it.get("Name") or "")
        quote_id = str(it.get("QuoteID") or "")  # 形如 1.688018（1=沪 0=深）
        stype_name = str(it.get("SecurityTypeName") or "")
        if not re.fullmatch(r"\d{6}", code) or "." not in quote_id:
            continue
        if stype_name and ("债" in stype_name or "基金" in stype_name or "退" in stype_name):
            continue
        market = 1 if quote_id.startswith("1.") else 0
        out.append({"code": code, "name": name, "market": market})
        if len(out) >= count:
            break
    return out


# ——— 黄金/期货（贵金属与商品期货主连）———
# 沪金/沪银走新浪期货连续合约（nf_AU0/nf_AG0），COMEX 走腾讯外盘（hf_GC/hf_SI），
# 其余（黄金T+D/焦炭）仍走东财兜底（无免费替代源时优雅占位）。
GOLD_FUTURES_SRC: dict[str, tuple[str, str, str]] = {
    "113.aum": ("sina", "nf_AU0", "沪金主连"),
    "113.agm": ("sina", "nf_AG0", "沪银主连"),
    "101.GC00Y": ("tencent", "hf_GC", "COMEX黄金"),
    "101.SI00Y": ("tencent", "hf_SI", "COMEX白银"),
    "118.AUTD": ("em", "", "黄金T+D"),
    "114.jm": ("em", "", "焦炭主连"),
}


def fetch_gold_futures() -> list[dict[str, Any]]:
    """贵金属/期货实时快照：沪金/沪银→新浪，COMEX→腾讯，其余→东财兜底。"""
    out: list[dict[str, Any]] = []
    for secid, (src, code, name) in GOLD_FUTURES_SRC.items():
        if src == "sina":
            out.append(_sina_future(code, name))
        elif src == "tencent":
            q = _tencent_future(code)
            q["name"] = name
            q.setdefault("code", secid)
            out.append(q)
        else:  # em
            eq = fetch_secid_quotes([secid])
            out.append(eq.get(secid) or {"code": secid, "name": name,
                                         "price": None, "pct": None, "chg": None})
    return out


# ── 多市场大盘指数 / 商品（参考东方财富行情中心，适配墨水屏）────────
# A 股指数走腾讯公开接口（fetch_quotes，已验证）；港股/美股走腾讯（hk/us 前缀，稳定）。
# 商品中 COMEX 金/银走腾讯外盘期货（hf_GC/hf_SI）；沪金/沪银(113) 走新浪期货连续合约
# （nf_AU0/nf_AG0），彻底避开被封锁的东方财富实时接口。
# 日经225(100.N225) 无免费替代源（腾讯/新浪均无），已从大盘指数分组移除。
# secid 规则：A 股 sh/sz + 6 位；港股/美股 100.xxx；期货/商品 113/101.xxx。
# 取不到行情的行显示占位（--），不阻断整屏渲染。
SECID_TO_TENCENT: dict[str, Optional[str]] = {
    "100.HSI": "hkHSI",
    "100.HSTECH": "hkHSTECH",
    "100.DJIA": "usDJI",
    "100.SPX": "usINX",
    "100.IXIC": "usIXIC",
    "101.GC00Y": "hf_GC",
    "101.SI00Y": "hf_SI",
    "101.CL00Y": "hf_CL",
    "101.OIL": "hf_OIL",
    # 以下无腾讯等价，仅走东财：沪金/沪银、日经
}


def _tencent_index(code: str) -> dict[str, Any]:
    """腾讯港股/美股指数（hkHSI / usDJI 等）。字段同 A 股：~ 分隔，[1]名称 [3]价 [31]涨跌额 [32]涨跌幅%。"""
    url = _QQ_QUOTE_URL + code
    try:
        req = urllib.request.Request(url, headers=_HEADERS)
        with _opener.open(req, timeout=_TIMEOUT) as resp:
            text = resp.read().decode("gbk", errors="replace")
    except Exception as exc:  # noqa: BLE001
        logger.warning("[FINANCE] tencent index failed %s: %s", code, exc)
        return {}
    m = re.search(r'v_%s="([^"]*)"' % re.escape(code), text)
    if not m:
        return {}
    parts = m.group(1).split("~")
    if len(parts) < 33:
        return {}
    return {
        "code": code,
        "name": parts[1],
        "price": _num(float(parts[3])) if parts[3] else None,
        "chg": _num(float(parts[31])) if parts[31] else None,
        "pct": _num(float(parts[32])) if parts[32] else None,
    }


def _tencent_future(code: str) -> dict[str, Any]:
    """腾讯外盘期货（hf_GC / hf_CL / hf_OIL / hf_SI）。逗号分隔：[0]最新价 [1]涨跌幅%。"""
    url = _QQ_QUOTE_URL + code
    try:
        req = urllib.request.Request(url, headers=_HEADERS)
        with _opener.open(req, timeout=_TIMEOUT) as resp:
            text = resp.read().decode("gbk", errors="replace")
    except Exception as exc:  # noqa: BLE001
        logger.warning("[FINANCE] tencent future failed %s: %s", code, exc)
        return {}
    m = re.search(r'v_%s="([^"]*)"' % re.escape(code), text)
    if not m:
        return {}
    parts = m.group(1).split(",")
    if len(parts) < 2:
        return {}
    return {
        "code": code,
        "name": parts[-1] if len(parts) > 12 else code,
        "price": _num(float(parts[0])) if parts[0] else None,
        "pct": _num(float(parts[1])) if parts[1] else None,
        "chg": None,
    }


def _fetch_with_tencent_fallback(secid: str, name: str,
                                 tcode: Optional[str]) -> dict[str, Any]:
    """单标的：腾讯主源，失败或缺失回落东财。显示名强制用配置名（屏蔽腾讯原始名）。"""
    q: dict[str, Any] = {}
    if tcode:
        q = _tencent_future(tcode) if tcode.startswith("hf") else _tencent_index(tcode)
        if not q.get("price"):
            q = {}
    if not q.get("price"):
        eq = fetch_secid_quotes([secid], {secid: name})
        q = eq.get(secid) or {"code": secid, "name": name,
                              "price": None, "pct": None, "chg": None}
    q["name"] = name
    q.setdefault("code", secid)
    q.setdefault("pct", None)
    q.setdefault("price", None)
    return q


# 沪金/沪银（上期所主连）新浪期货连续合约：东财被封锁时以此替代。
# 新浪 hq 字段：[6]=最新价 [27]=昨收，自算涨跌幅%。
SINA_FUTURES: dict[str, str] = {
    "113.aum": "nf_AU0",   # 沪金主连
    "113.agm": "nf_AG0",   # 沪银主连
}
_SINA_HQ_URL = "https://hq.sinajs.cn/list="


def _sina_future(sina_code: str, name: str) -> dict[str, Any]:
    """新浪期货连续合约（沪金 nf_AU0 / 沪银 nf_AG0）：[6]=最新价 [27]=昨收，自算涨跌幅%。"""
    empty = {"code": sina_code, "name": name, "price": None, "pct": None, "chg": None}
    try:
        req = urllib.request.Request(
            _SINA_HQ_URL + sina_code,
            headers={**_HEADERS, "Referer": "https://finance.sina.com.cn/"},
        )
        with _opener.open(req, timeout=_TIMEOUT) as resp:
            text = resp.read().decode("gbk", errors="replace")
    except Exception as exc:  # noqa: BLE001
        logger.warning("[FINANCE] sina future failed %s: %s", sina_code, exc)
        return empty
    m = re.search(r'"([^"]*)"', text)
    if not m:
        return empty
    parts = m.group(1).split(",")
    try:
        price = _num(float(parts[6]))
        prev = _num(float(parts[27])) if len(parts) > 27 and parts[27] else None
    except (ValueError, IndexError):
        return empty
    pct = ((price - prev) / prev * 100) if (price is not None and prev) else None
    return {"code": sina_code, "name": name, "price": price, "pct": pct, "chg": None}


MARKET_INDEX_GROUPS = [
    ("A股", ["sh000001", "sz399001", "sz399006", "sh000300", "sh000016"],
            ["上证指数", "深证成指", "创业板指", "沪深300", "上证50"]),
    ("港股", ["100.HSI", "100.HSTECH"], ["恒生指数", "恒生科技"]),
    ("美股", ["100.DJIA", "100.SPX", "100.IXIC"], ["道琼斯", "标普500", "纳斯达克"]),
]

COMMODITY_GROUPS = [
    ("贵金属", ["113.aum", "113.agm", "101.GC00Y", "101.SI00Y"],
              ["沪金", "沪银", "COMEX金", "COMEX银"]),
    ("石油", ["101.CL00Y", "101.OIL"], ["WTI原油", "布伦特原油"]),
]


def fetch_secid_quotes(secids: list[str], names: Optional[dict] = None) -> dict[str, dict]:
    """东财 ulist.np：按原始 secid 批量取实时快照，返回 {secid: quote}。"""
    if not secids:
        return {}
    url = (f"{_EM_QUOTE_URL}?fltt=2&invt=2&fields=f2,f3,f4,f12,f13,f14"
           f"&secids={','.join(secids)}")
    try:
        data = _get_json(url)
    except Exception:  # noqa: BLE001
        logger.warning("[FINANCE] secid quote failed secids=%s", secids)
        return {}
    diff = ((data or {}).get("data") or {}).get("diff") or []
    if isinstance(diff, dict):
        diff = list(diff.values())
    out: dict[str, dict] = {}
    for row in diff:
        mkt = row.get("f13")
        code = str(row.get("f12") or "")
        full = str(row.get("secid") or "")
        if not full and code:
            full = f"{mkt}.{code}" if mkt is not None else code
        if not full:
            continue
        out[full] = {
            "code": full,
            "name": ((names or {}).get(full) or str(row.get("f14") or "")),
            "price": _num(row.get("f2")),
            "pct": _num(row.get("f3")),
            "chg": _num(row.get("f4")),
            "market": int(mkt or 0),
        }
    return out


def fetch_market_indices() -> list[tuple[str, list[dict]]]:
    """分组返回多市场指数快照：[(分组名, [quote...]), ...]，quote 与 fetch_quotes 同构。"""
    result: list[tuple[str, list[dict]]] = []
    for grp, codes, names in MARKET_INDEX_GROUPS:
        name_map = dict(zip(codes, names))
        if grp == "A股":
            quotes = fetch_quotes(codes)  # 腾讯接口，已验证
            for q, nm in zip(quotes, names):
                q.setdefault("name", nm)
        else:
            quotes = []
            for sid, nm in zip(codes, names):
                quotes.append(_fetch_with_tencent_fallback(sid, nm, SECID_TO_TENCENT.get(sid)))
        result.append((grp, quotes))
    return result


def fetch_commodities() -> list[tuple[str, list[dict]]]:
    """分组返回贵金属/商品快照：[(分组名, [quote...]), ...]。

    沪金/沪银走新浪期货（nf_AU0/nf_AG0），其余（COMEX 等）走腾讯主源 + 东财兜底。
    """
    result: list[tuple[str, list[dict]]] = []
    for grp, codes, names in COMMODITY_GROUPS:
        quotes = []
        for sid, nm in zip(codes, names):
            if sid in SINA_FUTURES:
                quotes.append(_sina_future(SINA_FUTURES[sid], nm))
            else:
                quotes.append(_fetch_with_tencent_fallback(sid, nm, SECID_TO_TENCENT.get(sid)))
        result.append((grp, quotes))
    return result


def _fetch_sector_list(fs: str, limit: int = 8) -> list[dict]:
    """东财板块列表（行业 m:90+t:2 / 概念 m:90+t:3），返回按涨跌幅降序的前 N。f3 为百分比×100。"""
    url = (f"https://push2.eastmoney.com/api/qt/clist/get?pn=1&pz={limit}"
           f"&fs={urllib.parse.quote(fs)}&fields=f12,f14,f3,f128,f140,f141")
    try:
        data = _get_json(url)
    except Exception:  # noqa: BLE001
        logger.warning("[FINANCE] sector list unavailable fs=%s", fs)
        return []
    diff = ((data or {}).get("data") or {}).get("diff") or []
    if isinstance(diff, dict):
        diff = list(diff.values())
    rows = []
    for row in diff:
        pct_raw = _num(row.get("f3"))
        rows.append({
            "code": str(row.get("f12") or ""),
            "name": str(row.get("f14") or ""),
            "pct": (pct_raw / 100.0) if isinstance(pct_raw, float) else 0.0,
            "leader": str(row.get("f128") or ""),
        })
    rows.sort(key=lambda r: r["pct"], reverse=True)
    return rows[:limit]


def _parse_sina_bankuai(text: str) -> list[dict]:
    """解析新浪行业板块变量：每条「板块代码,名称,股票数,均价,涨跌额,涨跌幅%,...」，字段 5 即涨跌幅(%)。"""
    rows: list[dict] = []
    m = re.search(r"var S_Finance_bankuai_sinaindustry\s*=\s*\{(.*)\}\s*$", text, re.S)
    if not m:
        return rows
    for entry in re.finditer(r'"([^"]+)":"([^"]*)"', m.group(1)):
        vals = entry.group(2).split(",")
        if len(vals) < 6:
            continue
        try:
            pct = float(vals[5])
        except ValueError:
            continue
        rows.append({"name": vals[1], "pct": pct, "leader": vals[-1] if len(vals) > 12 else ""})
    rows.sort(key=lambda r: r["pct"], reverse=True)
    return rows


def _fetch_sector_list_sina() -> list[dict]:
    """新浪行业板块快照（newSinaHy.php）：name + 涨跌幅%。

    新浪接口稳定可达、无需鉴权，作为行业板块主源，彻底摆脱对东方财富的限流依赖。
    """
    global _SECTOR_SINA_CACHE
    now = time.time()
    if _SECTOR_SINA_CACHE and now - _SECTOR_SINA_CACHE[0] < _CACHE_TTL:
        return _SECTOR_SINA_CACHE[1]
    rows: list[dict] = []
    url = "https://money.finance.sina.com.cn/q/view/newSinaHy.php"
    req = urllib.request.Request(
        url,
        headers={**_HEADERS, "Referer": "https://finance.sina.com.cn/"},
    )
    ssl_ctx = ssl.create_default_context()
    ssl_ctx.check_hostname = False
    ssl_ctx.verify_mode = ssl.CERT_NONE
    last_exc: Optional[Exception] = None
    text = ""
    for _ in range(3):
        try:
            # 先经系统代理/直连（新浪对无代理直连偶发 reset，走默认 opener 更稳）；
            # 失败再试绕过代理的直连（生产环境避免 Clash 劫持）。
            try:
                with urllib.request.urlopen(req, timeout=12, context=ssl_ctx) as resp:
                    raw = resp.read()
            except Exception:  # noqa: BLE001
                with _opener.open(req, timeout=_TIMEOUT) as resp:
                    raw = resp.read()
            enc = resp.headers.get("Content-Encoding", "")
            if "gzip" in enc:
                raw = zlib.decompress(raw, 16 + zlib.MAX_WBITS)
            text = raw.decode("gbk", errors="replace")
            rows = _parse_sina_bankuai(text)
            if rows:
                break
        except Exception as exc:  # noqa: BLE001
            last_exc = exc
            time.sleep(0.8)
    if not rows:
        logger.warning("[FINANCE] sina bankuai failed: %s", last_exc)
        return []
    m = re.search(r"var S_Finance_bankuai_sinaindustry\s*=\s*\{(.*)\}\s*$", text, re.S)
    if not m:
        return []
    for entry in re.finditer(r'"([^"]+)":"([^"]*)"', m.group(1)):
        vals = entry.group(2).split(",")
        if len(vals) < 6:
            continue
        try:
            pct = float(vals[5])
        except ValueError:
            continue
        rows.append({"name": vals[1], "pct": pct, "leader": vals[-1] if len(vals) > 12 else ""})
    rows.sort(key=lambda r: r["pct"], reverse=True)
    _SECTOR_SINA_CACHE = (now, rows)
    return rows


def fetch_sectors() -> dict[str, list[dict]]:
    """板块动向：行业板块（新浪主源 + 东财兜底）。

    概念板块无稳定免费源（东财被封锁、新浪概念接口已下线），故 B 屏「更多行业」区
    直接复用新浪行业板块数据（扣除已展示的前 4 行），彻底去除对东方财富的依赖，
    保证 B 屏板块区域始终有数据、不依赖东财。
    """
    global _sectors_cache
    now = time.time()
    if _sectors_cache and now - _sectors_cache[0] < _CACHE_TTL:
        return _sectors_cache[1]
    industry = _fetch_sector_list_sina()
    if not industry:
        industry = _fetch_sector_list("m:90+t:2")  # 新浪不可达时回落东财
    # 新浪行业榜存在同名重复条目，按名称去重，避免 B 屏显示重复板块。
    seen: set[str] = set()
    deduped: list[dict] = []
    for r in industry:
        nm = r.get("name") or ""
        if nm in seen:
            continue
        seen.add(nm)
        deduped.append(r)
    industry = deduped
    # 「更多行业」区：复用新浪行业榜单，展示前 4 行之外的行业，避免与上方重复。
    concept = industry[4:8] if len(industry) > 4 else list(industry)
    result = {"industry": industry[:8], "concept": concept[:8]}
    _sectors_cache = (now, result)
    return result


# ── A 屏：大盘全景（1-bit 黑白，2 列网格） ───────────────────
def render_market_board(groups: list, w: int, h: int, title: str = "大盘全景",
                        colors: int = 2) -> Image.Image:
    """A 屏多市场指数总览。2 列网格，▲/▼ + 数值区分涨跌。

    黑白面板全黑字；三色/四色面板把上涨项标红（A 股习惯），下跌保持黑色。
    """
    colored = colors >= 3
    img = _palette(w, h, colors) if colored else Image.new("1", (w, h), 1)
    draw = ImageDraw.Draw(img)
    s = w / 400.0
    pad = int(10 * s)
    f_title = _cjk(max(14, int(18 * s)))
    f_meta = _cjk(max(9, int(11 * s)))
    f_name = _cjk(max(10, int(12 * s)))
    f_val = _cjk(max(10, int(12 * s)))

    today = datetime.now().strftime("%m-%d %H:%M")
    draw.text((pad, int(8 * s)), title, fill=0, font=f_title)
    tw = draw.textlength(today, font=f_meta)
    draw.text((w - pad - tw, int(12 * s)), today, fill=0, font=f_meta)
    draw.line([(pad, int(34 * s)), (w - pad, int(34 * s))], fill=0, width=max(1, int(s)))

    items = [(g, q) for g, rows in groups for q in rows]
    cols = 2
    rows_n = max(1, (len(items) + cols - 1) // cols)
    top = int(40 * s)
    bottom = h - int(26 * s)  # 抬高底部，给页脚留出充足间距，避免末行拥挤
    row_h = (bottom - top) / rows_n
    col_w = (w - 2 * pad) / cols
    for idx, (g, q) in enumerate(items):
        r = idx // cols
        c = idx % cols
        col_left = pad + c * col_w
        col_center = col_left + col_w / 2
        y = top + r * row_h
        cy = y + row_h / 2
        name = str(q.get("name") or q.get("code") or "--")
        price = _fmt(q.get("price"))
        pct = _pct_text(q.get("pct"))
        val_line = f"{price}  {pct}"
        # 每列内容在其半区内水平居中，使中线两侧视觉对称、分割线显得居中
        nw = draw.textlength(name, font=f_name)
        vw = draw.textlength(val_line, font=f_val)
        nx = max(col_left + int(3 * s), min(col_center - nw / 2, col_left + col_w - int(3 * s) - nw))
        vx = max(col_left + int(3 * s), min(col_center - vw / 2, col_left + col_w - int(3 * s) - vw))
        draw.text((nx, cy - row_h * 0.42), name, fill=0, font=f_name)
        # 涨跌整行上色：三色/四色面板涨红、跌黑；黑白面板恒黑。
        draw.text((vx, cy + int(2 * s)), val_line,
                  fill=3 if (colored and _is_up(q)) else 0, font=f_val)
    # 列分隔线（仅当双列且行数 > 1）；自页眉线起向下贯通，框出网格
    if rows_n > 1:
        draw.line([(pad + col_w, int(34 * s)), (pad + col_w, bottom)], fill=0, width=1)
    foot = "行情快照 · 多源聚合（原型）"
    fw = draw.textlength(foot, font=f_meta)
    draw.text((w - pad - fw, h - int(12 * s)), foot, fill=0, font=f_meta)
    return img


# ── B 屏：板块动向 + 贵金属/石油（BWRY 四色，红涨黑跌） ─────────
def render_sectors(sectors: dict, commodities: list, w: int, h: int, colors: int) -> Image.Image:
    """B 屏板块动向（红涨黑跌）+ 贵金属/石油小卡。

    三色面板（BWR）用红色做唯一强调色；四色面板（BWRY）改用黄色做版式强调，
    红色只留给涨幅。这样同一套模板在两种面板上都能出图，且一眼能分出是哪版。
    """
    is_color = colors >= 3
    img = _palette(w, h, colors) if is_color else Image.new("1", (w, h), 1)
    draw = ImageDraw.Draw(img)
    s = w / 400.0
    pad = int(10 * s)
    f_title = _load_cjk_bold(max(14, int(17 * s)))
    f_meta = _cjk(max(9, int(10 * s)))
    f_sec = _cjk(max(10, int(12 * s)))
    f_bar = _cjk(max(9, int(11 * s)))
    fg = 0
    red = 3 if is_color else 0
    accent = 2 if is_color else 0  # 三色=红，四色=黄

    today = datetime.now().strftime("%m-%d %H:%M")
    draw.rectangle([pad, int(10 * s), pad + int(3 * s), int(31 * s)], fill=accent)
    draw.text((pad + int(8 * s), int(8 * s)), "板块动向", fill=fg, font=f_title)
    tw = draw.textlength(today, font=f_meta)
    draw.text((w - pad - tw, int(12 * s)), today, fill=fg, font=f_meta)
    draw.line([(pad, int(34 * s)), (w - pad, int(34 * s))], fill=fg, width=max(1, int(s)))

    def _section_label(text: str, y: int) -> None:
        draw.rectangle([pad, y + int(3 * s), pad + int(3 * s), y + int(11 * s)], fill=accent)
        draw.text((pad + int(7 * s), y), text, fill=fg, font=f_sec)

    y = int(42 * s)
    _section_label("行业板块", y)
    y += int(18 * s)
    y = _draw_sector_rows(draw, (sectors.get("industry") or [])[:4], pad, y, w, s, f_bar, red, fg)
    y += int(3 * s)
    _section_label("更多行业", y)
    y += int(18 * s)
    y = _draw_sector_rows(draw, (sectors.get("concept") or [])[:3], pad, y, w, s, f_bar, red, fg)
    y += int(3 * s)
    _section_label("贵金属 · 石油", y)
    y += int(16 * s)
    _draw_commodity_strip(draw, commodities, pad, y, w, s, f_bar, red, fg)
    foot = "板块/商品 · 多源聚合（原型）"
    fw = draw.textlength(foot, font=f_meta)
    draw.text((w - pad - fw, h - int(11 * s)), foot, fill=fg, font=f_meta)
    return img


def _draw_sector_rows(draw, rows, pad, y, w, s, font, red, fg):
    if not rows:
        draw.text((pad + int(6 * s), y), "板块数据同步中…", fill=fg, font=font)
        return y + int(20 * s)
    bar_x = pad + int(64 * s)
    bar_max = (w - pad) - bar_x - int(46 * s)
    mid_x = bar_x + bar_max // 2
    row_h = int(20 * s)
    for r in rows:
        name = str(r.get("name") or "")[:6]
        pct = r.get("pct") or 0.0
        col = red if pct >= 0 else 0
        draw.text((pad, y), name, fill=fg, font=font)
        mid = y + row_h // 2
        draw.line([(bar_x, mid), (bar_x + bar_max, mid)], fill=fg, width=1)
        extent = max(-1.0, min(1.0, pct / 5.0)) * (bar_max / 2)
        if extent >= 0:
            draw.rectangle([mid_x, mid - int(2 * s), int(mid_x + extent), mid + int(2 * s)], fill=col)
        else:
            draw.rectangle([int(mid_x + extent), mid - int(2 * s), mid_x, mid + int(2 * s)], fill=col)
        arrow = "▲" if pct >= 0 else "▼"
        draw.text((bar_x + bar_max + int(4 * s), y), f"{arrow}{abs(pct):.2f}%", fill=col, font=font)
        y += row_h
    return y + int(2 * s)


def _draw_commodity_strip(draw, commodities, pad, y, w, s, font, red, fg):
    items = []
    for g, rows in commodities:
        for q in rows:
            items.append((g, q))
    if not items:
        draw.text((pad + int(6 * s), y), "暂无商品数据", fill=fg, font=font)
        return
    n = len(items)
    cell_w = (w - 2 * pad) / n
    for i, (g, q) in enumerate(items):
        x = pad + i * cell_w
        name = str(q.get("name") or q.get("code") or "--")
        pct = q.get("pct")
        col = red if (pct or 0) >= 0 else 0
        draw.text((x, y), name, fill=fg, font=font)
        draw.text((x, y + int(12 * s)), _fmt(q.get("price")), fill=fg, font=font)
        draw.text((x, y + int(24 * s)), _pct_text(pct), fill=col, font=font)
