"""番茄钟引擎（clean-room 实现）。

只借鉴「番茄工作法」这一公开方法：专注 N 分钟 → 短休 M 分钟，每 K 轮改写一次长休。
不复制任何第三方固件代码；原工程（Rukenshia/pomodoro）无 LICENSE，仅作产品形态参考。

设计要点（也是与墨水屏适配的关键）：

1. **状态只有绝对时间戳**。会话里存 `started_at` + `duration_s`，剩余时间一律用
   ``now`` 现算，绝不存「剩余秒数」递减。设备离线、服务重启、跨天、跨时区都不会走偏，
   重新上线时自动对齐——这是 e-ink 这种「按需刷新」设备唯一的正确做法。
2. **暂停不改锚点**。暂停只记 `paused_at`，恢复时把暂停时长累加到 `paused_total_s`，
   `started_at` 保持不变，因此 `end_at = started_at + duration + paused_total` 永远可推导。
3. **纯函数**。本模块不碰数据库、不碰 IO，输入 session + now，输出视图模型，
   渲染层（render.py）与控制接口（main.py）各自消费同一份结果。
4. **双屏视图分离**。``screen_a`` 只出「阶段 + 大字号剩余 + 结束时刻」，信息量小、
   每分钟变一次；``screen_b`` 出今日统计，变化更慢。两屏刷新节奏可以不同。
"""
from __future__ import annotations

from dataclasses import asdict, dataclass
from datetime import UTC, datetime, timedelta
from typing import Any, Optional

# 阶段
FOCUS = "focus"
SHORT_BREAK = "short_break"
LONG_BREAK = "long_break"

# 会话状态
RUNNING = "running"
PAUSED = "paused"
IDLE = "idle"

PHASE_LABEL = {FOCUS: "专注", SHORT_BREAK: "短休息", LONG_BREAK: "长休息"}


@dataclass(frozen=True)
class Preset:
    """一组番茄参数。时长单位为秒，``long_break_after`` 为「几轮专注后长休」。"""

    key: str
    name: str
    focus_s: int
    short_break_s: int
    long_break_s: int
    long_break_after: int = 4
    hint: str = ""

    def public(self) -> dict[str, Any]:
        return {
            "key": self.key,
            "name": self.name,
            "focus_minutes": self.focus_s // 60,
            "short_break_minutes": self.short_break_s // 60,
            "long_break_minutes": self.long_break_s // 60,
            "long_break_after": self.long_break_after,
            "hint": self.hint,
        }


PRESETS: dict[str, Preset] = {
    "focus": Preset("focus", "专注", 25 * 60, 5 * 60, 20 * 60, 4, "经典 25 / 5"),
    "coding": Preset("coding", "写代码", 45 * 60, 15 * 60, 30 * 60, 2, "长时段 45 / 15"),
    "email": Preset("email", "处理邮件", 15 * 60, 5 * 60, 15 * 60, 4, "短时段 15 / 5"),
}
DEFAULT_PRESET = "focus"


def preset_public() -> list[dict[str, Any]]:
    return [p.public() for p in PRESETS.values()]


def resolve_preset(key: str | None) -> Preset:
    return PRESETS.get(str(key or DEFAULT_PRESET), PRESETS[DEFAULT_PRESET])


def preset_from_config(cfg: dict[str, Any] | None) -> Preset:
    """后台配置优先于内置预设。

    内容后台可以把 ``preset`` 写成 dict（自定义时长）或 preset key（内置预设）。
    自定义时缺省字段回落到所选内置预设，避免「只改了专注时长，休息变 0 分钟」。
    """
    cfg = cfg or {}
    raw = cfg.get("preset")
    if isinstance(raw, dict):
        base = resolve_preset(raw.get("key") or cfg.get("presetKey"))
        return Preset(
            key=str(raw.get("key") or base.key),
            name=str(raw.get("name") or base.name),
            focus_s=_minutes(raw.get("focus_minutes"), base.focus_s // 60) * 60,
            short_break_s=_minutes(raw.get("short_break_minutes"), base.short_break_s // 60) * 60,
            long_break_s=_minutes(raw.get("long_break_minutes"), base.long_break_s // 60) * 60,
            long_break_after=max(1, _minutes(raw.get("long_break_after"), base.long_break_after)),
            hint=str(raw.get("hint") or base.hint),
        )
    return resolve_preset(raw or cfg.get("presetKey"))


def _minutes(value: Any, fallback: int) -> int:
    try:
        n = int(value)
    except (TypeError, ValueError):
        return int(fallback)
    return n if positive(n) else int(fallback)


def positive(n: int) -> bool:
    return n > 0


# ── 时间助手 ────────────────────────────────────────────────
def now_utc() -> datetime:
    return datetime.now(UTC)


def parse_ts(value: Any) -> Optional[datetime]:
    if not value:
        return None
    try:
        dt = datetime.fromisoformat(str(value))
    except ValueError:
        return None
    return dt.replace(tzinfo=UTC) if dt.tzinfo is None else dt.astimezone(UTC)


def iso(dt: datetime | None) -> str | None:
    return dt.astimezone(UTC).isoformat() if dt else None


# ── 会话构造与推进 ──────────────────────────────────────────
def phase_duration(preset: dict[str, Any], phase: str) -> int:
    if phase == FOCUS:
        return int(preset.get("focus_s") or 25 * 60)
    if phase == LONG_BREAK:
        return int(preset.get("long_break_s") or 20 * 60)
    return int(preset.get("short_break_s") or 5 * 60)


def new_session(device_id: str, preset: Preset, now: datetime | None = None) -> dict[str, Any]:
    """开始一轮专注。``cycle_index`` 从 1 计，表示「今天这一组的第几个番茄」。"""
    now = now or now_utc()
    return {
        "device_id": device_id,
        "preset_key": preset.key,
        "preset": asdict(preset),
        "phase": FOCUS,
        "status": RUNNING,
        "started_at": iso(now),
        "duration_s": preset.focus_s,
        "cycle_index": 1,
        "paused_at": None,
        "paused_total_s": 0,
        "updated_at": iso(now),
    }


def advance(session: dict[str, Any], now: datetime | None = None, *, skipped: bool = False) -> dict[str, Any]:
    """进入下一阶段：专注 → 休息（满 N 轮为长休），休息 → 专注（轮次 +1）。

    ``skipped=True`` 表示用户主动跳过（此时不给休息计数加番茄）。
    """
    now = now or now_utc()
    preset = session.get("preset") or {}
    phase = session.get("phase") or FOCUS
    cycle = int(session.get("cycle_index") or 1)
    after = max(1, int(preset.get("long_break_after") or 4))

    if phase == FOCUS:
        next_phase = LONG_BREAK if cycle >= after else SHORT_BREAK
        next_cycle = cycle
    else:
        next_phase = FOCUS
        next_cycle = 1 if phase == LONG_BREAK else min(cycle + 1, after)

    out = dict(session)
    out.update({
        "phase": next_phase,
        "status": RUNNING,
        "started_at": iso(now),
        "duration_s": phase_duration(preset, next_phase),
        "cycle_index": next_cycle,
        "paused_at": None,
        "paused_total_s": 0,
        "updated_at": iso(now),
    })
    if skipped:
        out["skipped"] = True
    return out


def pause(session: dict[str, Any], now: datetime | None = None) -> dict[str, Any]:
    if session.get("status") != RUNNING:
        return session
    now = now or now_utc()
    out = dict(session)
    out.update({"status": PAUSED, "paused_at": iso(now), "updated_at": iso(now)})
    return out


def resume(session: dict[str, Any], now: datetime | None = None) -> dict[str, Any]:
    if session.get("status") != PAUSED:
        return session
    now = now or now_utc()
    paused_at = parse_ts(session.get("paused_at"))
    extra = int((now - paused_at).total_seconds()) if paused_at else 0
    out = dict(session)
    out.update({
        "status": RUNNING,
        "paused_at": None,
        "paused_total_s": int(session.get("paused_total_s") or 0) + max(0, extra),
        "updated_at": iso(now),
    })
    return out


def stop(device_id: str, preset: Preset | dict[str, Any], now: datetime | None = None) -> dict[str, Any]:
    now = now or now_utc()
    preset_dict = asdict(preset) if isinstance(preset, Preset) else dict(preset or {})
    return {
        "device_id": device_id,
        "preset_key": str(preset_dict.get("key") or DEFAULT_PRESET),
        "preset": preset_dict,
        "phase": FOCUS,
        "status": IDLE,
        "started_at": iso(now),
        "duration_s": phase_duration(preset_dict, FOCUS),
        "cycle_index": 1,
        "paused_at": None,
        "paused_total_s": 0,
        "updated_at": iso(now),
    }


# ── 视图模型 ────────────────────────────────────────────────
def view(session: dict[str, Any] | None, now: datetime | None = None) -> dict[str, Any]:
    """把会话折算成「此刻该怎么显示」。纯函数，可任意频率调用。"""
    now = now or now_utc()
    if not session:
        preset = PRESETS[DEFAULT_PRESET]
        return {
            "status": IDLE,
            "phase": FOCUS,
            "phase_label": "未开始",
            "preset_name": preset.name,
            "remaining_s": preset.focus_s,
            "duration_s": preset.focus_s,
            "clock": _clock(preset.focus_s),
            "elapsed_s": 0,
            "progress": 0,
            "over": False,
            "cycle_index": 1,
            "cycle_total": preset.long_break_after,
            "started_at": None,
            "end_at": None,
            "window": "",
            "next_label": f"休息 {preset.short_break_s // 60} 分钟",
            "over_hint": "",
            "ended_minutes_ago": 0,
        }

    preset = session.get("preset") or {}
    phase = str(session.get("phase") or FOCUS)
    status = str(session.get("status") or IDLE)
    duration = max(1, int(session.get("duration_s") or 1))
    started = parse_ts(session.get("started_at"))
    paused_total = int(session.get("paused_total_s") or 0)

    if status == IDLE or not started:
        elapsed = 0.0
        end_at = None
    elif status == PAUSED:
        # 暂停计时冻结：把「暂停那一刻」当成 now，锚点与累计暂停时长都不变，
        # 因此 end_at 仍可用 started + duration + paused_total 推导。
        paused_at = parse_ts(session.get("paused_at")) or started
        elapsed = (paused_at - started).total_seconds() - paused_total
        end_at = started + timedelta(seconds=duration + paused_total)
    else:
        elapsed = (now - started).total_seconds() - paused_total
        end_at = started + timedelta(seconds=duration + paused_total)

    elapsed = max(0.0, min(float(duration), elapsed))
    remaining = max(0, int(round(duration - elapsed)))
    over = elapsed >= duration - 0.5 and status == RUNNING
    progress = int(round(elapsed / duration * 100)) if duration else 0

    cycle = max(1, int(session.get("cycle_index") or 1))
    cycle_total = max(1, int(preset.get("long_break_after") or 4))

    if status == PAUSED:
        phase_label = "已暂停"
    elif over:
        phase_label = "专注结束" if phase == FOCUS else "休息结束"
    else:
        phase_label = "专注中" if phase == FOCUS else ("长休息" if phase == LONG_BREAK else "短休息")

    if phase == FOCUS:
        is_long_next = cycle >= cycle_total
        next_label = (f"长休息 {int(preset.get('long_break_s') or 0) // 60} 分钟"
                      if is_long_next else
                      f"休息 {int(preset.get('short_break_s') or 0) // 60} 分钟")
        over_hint = f"该休息了 · {next_label}"
    else:
        next_label = f"回到专注 {int(preset.get('focus_s') or 0) // 60} 分钟"
        over_hint = "该回到专注了"

    return {
        "status": status,
        "phase": phase,
        "phase_label": phase_label,
        "preset_name": str(preset.get("name") or "专注"),
        "remaining_s": remaining,
        "duration_s": duration,
        "clock": _clock(remaining),
        "elapsed_s": int(elapsed),
        "progress": progress,
        "over": over,
        "cycle_index": cycle,
        "cycle_total": cycle_total,
        "started_at": iso(started),
        "end_at": iso(end_at),
        "window": f"{_hm(started)} → {_hm(end_at)}" if started and end_at else "",
        "next_label": next_label,
        "over_hint": over_hint,
        "ended_minutes_ago": int((now - end_at).total_seconds() // 60) if (over and end_at) else 0,
    }


def screen_a(v: dict[str, Any]) -> dict[str, Any]:
    """A 屏（黑白）：一屏一个焦点，字号最大，每分钟只变一个数字。"""
    if v["status"] == IDLE:
        return {"kicker": "番茄钟", "title": v["preset_name"], "clock": "--:--",
                "cycle": "在手机上点开始", "hint": f"{v['duration_s'] // 60} 分钟一组"}
    if v["status"] == PAUSED:
        hint = "点击继续"
    elif v["over"]:
        hint = v["over_hint"]
    else:
        hint = v["window"]
    return {
        "kicker": v["phase_label"],
        "title": v["preset_name"],
        "clock": v["clock"],
        "cycle": f"第 {v['cycle_index']}/{v['cycle_total']} 轮",
        "hint": hint,
    }


def screen_b(v: dict[str, Any], stats: dict[str, Any] | None = None) -> dict[str, Any]:
    """B 屏（四色）：今日统计，顺手把当前进度画成一条进度条。"""
    stats = stats or {}
    count = int(stats.get("focus_count") or 0)
    seconds = int(stats.get("focus_seconds") or 0)
    return {
        "title": "今日专注",
        "count_text": f"{count} 个番茄",
        "time_text": _duration_text(seconds) or "0 分钟",
        "bar_text": f"第 {v['cycle_index']}/{v['cycle_total']} 轮",
        "progress": v["progress"],
        "next": v["next_label"],
        "footer": v["phase_label"],
    }


def _clock(seconds: int) -> str:
    seconds = max(0, int(seconds))
    return f"{seconds // 60:02d}:{seconds % 60:02d}"


def _hm(dt: datetime | None) -> str:
    return dt.astimezone().strftime("%H:%M") if dt else "--:--"


def _duration_text(seconds: int) -> str:
    seconds = max(0, int(seconds))
    hours, minutes = divmod(seconds // 60, 60)
    if hours and minutes:
        return f"{hours}h {minutes}m"
    if hours:
        return f"{hours}h"
    return f"{minutes} 分钟" if minutes else ""
