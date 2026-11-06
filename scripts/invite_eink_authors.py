#!/usr/bin/env python3
"""墨水屏开源作者邀请：自动找人 / 查资格 / 写个性化草稿，发送受控。

为什么不是「一把梭群发」：

GitHub 可接受使用政策把「批量未经请求的招揽」（mass unsolicited solicitation）
明确列为滥用。一天给几十个仓库开 issue 的结果，按经验从轻到重是：
被限流 → 账号被标 spam、issue 被批量关闭 → 身份被平台挂起。而我们这个账号
（upisqueen-blip）刚在上游提了 PR #3 和 issue #4，那两条是真实的技术资产，
不能拿去赌。

所以脚本把「自动」放在前半段——搜索、查资格、拼证据、写草稿、去重、记账；
把「发送」做成受控动作：

  · 默认 dry-run，只产出草稿，不发
  · 加 --send 才真发，且每次最多 3 条、每天最多 5 条
  · 同一作者（owner）只发一次；发过 / 被拒绝过的仓库永不再发
  · 发之前再做一次「我们是否已经在这个仓库里出现过」的深度检查
  · 每条之间隔 45 秒，不制造突发流量

用法：

    # 1) 先跑筛选（若还没有名单）
    python scripts/find_eink_projects.py --days 180

    # 2) 生成草稿，人工过一眼（默认就是这一步）
    python scripts/invite_eink_authors.py --limit 12

    # 3) 确认没问题后再发，一次 3 条
    python scripts/invite_eink_authors.py --send --max-per-run 3

    # 4) 只想发某一个（推荐：先手发一两个看反应）
    python scripts/invite_eink_authors.py --send --only usetrmnl/trmnl-firmware

    # 5) 验证 token 与发送链路是否可用（发到自己的测试仓库）
    python scripts/invite_eink_authors.py --send --test-repo your-name/sandbox
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from find_eink_projects import GitHub, Project  # noqa: E402

# 身份与落地页。改这里就能换口径，别散落在正文模板里。
HOME_URL = "https://dual.xiaowuai.vip/"
TRY_URL = "https://dual.xiaowuai.vip/try"

# 硬上限：这两个数字是给「账号安全」用的，不是运营指标。
DEFAULT_MAX_PER_RUN = 3
DEFAULT_MAX_PER_DAY = 5
DEFAULT_DELAY_S = 45

CJK_RE = re.compile(r"[\u4e00-\u9fff]")


# ---------------------------------------------------------------------------
# gh 调用
# ---------------------------------------------------------------------------
def run_gh(args: list[str], payload: str | None = None, timeout: int = 180) -> str:
    proc = subprocess.run(["gh", *args], input=payload, capture_output=True,
                          text=True, encoding="utf-8", timeout=timeout)
    if proc.returncode != 0:
        raise RuntimeError((proc.stderr or proc.stdout or "").strip()[:400])
    return proc.stdout.strip()


def gh_json(args: list[str], payload: str | None = None) -> dict:
    out = run_gh(args, payload)
    return json.loads(out or "{}")


def viewer_login() -> str:
    return gh_json(["api", "user"]).get("login") or ""


def http_status(url: str) -> int:
    try:
        req = urllib.request.Request(url, method="GET", headers={"User-Agent": "xiaowu-invite/1.0"})
        with urllib.request.urlopen(req, timeout=20) as resp:
            return resp.status
    except urllib.error.HTTPError as exc:
        return exc.code
    except Exception:  # noqa: BLE001
        return 0


# ---------------------------------------------------------------------------
# 资格核查
# ---------------------------------------------------------------------------
# 资格核查结果按仓库名缓存，供后面写草稿时判断「有没有 issue 模板 / 有没有开 Discussion」
p_meta: dict[str, "Meta"] = {}


@dataclass
class Meta:
    full_name: str
    archived: bool = False
    issues: bool = True
    discussions: bool = False
    owner_type: str = ""
    open_issues: int = 0
    has_template: bool = False
    blocker: str = ""

    @property
    def eligible(self) -> bool:
        return not self.blocker


def fetch_meta(repos: list[str], batch: int = 20) -> dict[str, Meta]:
    """一批 GraphQL 把归档 / issue 开关 / Discussions / 账号类型取完。

    这些决定「发过去会不会直接被关掉」，比分数重要。
    """
    out: dict[str, Meta] = {}
    for start in range(0, len(repos), batch):
        chunk = repos[start:start + batch]
        parts = []
        for i, full in enumerate(chunk):
            owner, _, name = full.partition("/")
            owner = re.sub(r"[^A-Za-z0-9._-]", "", owner)
            name = re.sub(r"[^A-Za-z0-9._-]", "", name)
            if not owner or not name:
                continue
            parts.append(
                f'r{i}:repository(owner:"{owner}",name:"{name}"){{'
                f' nameWithOwner isArchived hasIssuesEnabled hasDiscussionsEnabled'
                f' owner{{ __typename }}'
                f' issues(states:OPEN){{ totalCount }} }}'
            )
        if not parts:
            continue
        try:
            data = gh_json(["api", "graphql", "--input", "-"],
                           json.dumps({"query": "query{" + " ".join(parts) + "}"})).get("data") or {}
        except Exception as exc:  # noqa: BLE001
            print(f"  [!] 资格核查第 {start // batch + 1} 批失败：{exc}")
            continue
        for i, full in enumerate(chunk):
            node = data.get(f"r{i}")
            meta = Meta(full_name=full)
            if not node:
                meta.blocker = "仓库不存在或已改名"
                out[full] = meta
                continue
            meta.archived = bool(node.get("isArchived"))
            meta.issues = bool(node.get("hasIssuesEnabled"))
            meta.discussions = bool(node.get("hasDiscussionsEnabled"))
            meta.owner_type = ((node.get("owner") or {}).get("__typename")) or ""
            meta.open_issues = int(((node.get("issues") or {}).get("totalCount")) or 0)
            if meta.archived:
                meta.blocker = "仓库已归档"
            elif not meta.issues:
                meta.blocker = "仓库关闭了 issue（只能走 Discussion 或 PR）"
            out[full] = meta
        time.sleep(0.3)
    return out


def has_issue_template(repo: str) -> bool:
    """有 issue 模板的仓库，自由格式的 issue 大概率被 bot 自动关掉。"""
    try:
        gh_json(["api", f"repos/{repo}/contents/.github/ISSUE_TEMPLATE"])
        return True
    except Exception:  # noqa: BLE001
        return False


def already_involved(repo: str, me: str) -> str:
    """我们是否已经在这个仓库出现过（提过 PR / 发过 issue / 被 @ 过）。

    返回空串表示干净。用 search 的 involves: 而不是 author:，
    因为之前给 pomodoro 提 PR 时在 issue 里留过评论，author: 查不到。
    """
    q = urllib.parse.quote(f"repo:{repo} involves:{me}")
    try:
        data = gh_json(["api", f"search/issues?q={q}&per_page=1"])
    except Exception:  # noqa: BLE001
        return ""  # 查不动就别拦，交给人工判断
    total = int(data.get("total_count") or 0)
    if total:
        items = data.get("items") or []
        hint = ""
        if items:
            first = items[0]
            hint = f"（{first.get('html_url')}）"
        return f"已有 {total} 条我们的痕迹{hint}"
    return ""


# ---------------------------------------------------------------------------
# 草稿生成
# ---------------------------------------------------------------------------
def _pick_lang(p: Project, forced: str) -> str:
    if forced != "auto":
        return forced
    blob = " ".join([p.description or "", p.name or ""])
    return "zh" if CJK_RE.search(blob) else "en"


def _evidence_path(p: Project, key: str) -> str:
    """证据里可能是一句 README 摘录，只认看起来像路径的。"""
    raw = (p.evidence or {}).get(key) or ""
    raw = raw.strip().strip("`")
    return raw if re.search(r"[/\\.]", raw) and " " not in raw else ""


def _opening(p: Project, lang: str) -> str:
    drv = _evidence_path(p, "driver")
    dat = _evidence_path(p, "data")
    rnd = _evidence_path(p, "render")
    caps = p.caps or {}
    if lang == "zh":
        if caps.get("driver") and drv:
            return f"`{drv}` 这段我读得最细——面板初始化序列写得很短，注释也够，比大多数同类实现好读。"
        if caps.get("data") and dat:
            return f"`{dat}` 把取数和绘制分开了，这一点是关键：接的时候我一行都没动你的排版代码。"
        if caps.get("render") and rnd:
            return f"`{rnd}` 把排版和最后的刷屏分开，换输出面板只是换最后一步。"
        return "工程结构很清楚，接起来没花什么力气。"
    if caps.get("driver") and drv:
        return (f"`{drv}` is the part I read closest — the panel init sequence is short "
                f"and readable, which is rarer than it sounds.")
    if caps.get("data") and dat:
        return (f"how `{dat}` separates fetching from drawing is the reason this worked: "
                f"I never had to touch your layout code.")
    if caps.get("render") and rnd:
        return (f"`{rnd}` keeps layout separate from the final blit, so swapping the output "
                f"panel is one step instead of a rewrite.")
    return "the structure is easy to follow."


def _fit(p: Project, lang: str) -> str:
    caps = p.caps or {}
    name = p.name
    if lang == "zh":
        if caps.get("driver") and caps.get("data"):
            return (f"所以「{name}」的缺口很小：动屏的地方集中在一处，值本来就是现成的数据，"
                    f"加一路副屏输出是叠加，不是重写。")
        if caps.get("driver"):
            return (f"所以「{name}」的缺口很小：写屏已经收在一层里，加副屏应该碰不到你的界面代码。")
        if caps.get("data"):
            return (f"所以「{name}」其实不需要写驱动：JSON 那条路直接返回 400×300 的位图，"
                    f"对你来说只是多一种输出格式。")
        return f"所以「{name}」里最有价值的排版逻辑可以整套留着，只需要换掉最后刷屏那一步。"
    if caps.get("driver") and caps.get("data"):
        return (f"So for {name} the gap is small: everything that touches the panel goes through "
                f"one place, and the values are already plain data — a second sink is additive, "
                f"not a rewrite.")
    if caps.get("driver"):
        return (f"So for {name} the gap is small: the panel writes already sit behind one layer, "
                f"so a second sink shouldn't reach your UI code at all.")
    if caps.get("data"):
        return (f"So for {name} you wouldn't need a driver at all: the JSON path returns a "
                f"400×300 frame directly, so it's one more output format next to what you have.")
    return (f"So for {name} the layout work is the valuable half — the JSON path only replaces "
            f"the last step, the blit.")


COMMERCIAL_RE = re.compile(
    r"(shop|store|pricing|buy now|purchase|for sale|order now|commercial|"
    r"商城|店铺|购买|商用|量产|官网有售)", re.I)


def commercial_hint(p: Project) -> str:
    """提示「这可能是个在卖产品的团队」——大概率是同行，不是想找地方上架的作者。"""
    blob = " ".join([p.description or "", p.name or "", " ".join(p.topics or [])])
    if COMMERCIAL_RE.search(blob):
        return "简介像在卖产品（可能是同行）"
    if p.owner.lower() in {"usetrmnl", "visionect", "eink", "waveshare", "seeedstudio",
                           "solderedelectronics", "inkplate"}:
        return "厂商账号（同行）"
    return ""


def _title(p: Project, lang: str) -> str:
    if lang == "zh":
        return f"给 {p.name} 加一块 400×300 三色副屏，值得开个分支吗？"
    return f"Second 400×300 panel for {p.name} — worth a branch?"


def build_draft(p: Project, lang: str, me: str, try_url: str, home_url: str) -> tuple[str, str]:
    opening = _opening(p, lang)
    fit = _fit(p, lang)
    name = p.name

    if lang == "zh":
        body = f"""你好，

{opening}

我在做一块小墨水屏设备：ESP32-S3，两块 400×300 并排，一块黑白一块三色。问题是每个想跑在上面的项目都得重新写一遍显示驱动，这个交换不划算。所以我把渲染放到了服务端——推一段 JSON，拿回两块屏各自的完整帧：

    {{"version":1,"title":"API 额度","items":[{{"label":"剩余","value":"$42.10"}}]}}

帧就是 400×300：黑白屏 1 bit、三色屏 2 bit，按行打包，拿到就能直接塞进你现有的缓冲区。

有个试出图的页面，不用注册、不用 key：{try_url}

{fit}

有意思的话我可以提一个分支，加一路副屏输出，你现在的路径一行不动，合不合回由你判断，我不催。

说明一下：这是我做的商业产品，不是随手路过。如果「{name}」上了我们的社区页，会链回你的仓库，代码和许可证都保持原样。

如果不希望收到这类消息，回一句就行，我不会再打扰。

— {me}
{home_url}
"""
    else:
        body = f"""Hi,

{opening}

I'm building a small e-ink device: an ESP32-S3 with two 400×300 panels side by side, one black/white and one tri-colour. Right now every project that wants to run on it has to write another display driver, which is a bad trade for everyone. So I moved the rendering to the server instead — POST a JSON card, get back the exact frame for both panels:

    {{"version":1,"title":"API quota","items":[{{"label":"Left","value":"$42.10"}}]}}

The frame is 400×300 — 1-bit for the B/W panel, 2-bit for the tri-colour one — packed row-major, so it drops straight into the buffer you already have.

There's a try page, no account and no key: {try_url}

{fit}

If that's interesting I can send a branch that adds a second-panel sink and leaves your current path untouched — you decide whether it's worth merging, and I won't chase it.

One disclosure: this is a commercial device I'm building, so this isn't a drive-by. If {name} gets listed on our community page it links back to your repo, and your code and licence stay exactly as they are.

If you'd rather not get messages like this, say so and I won't contact you again.

— {me}
{home_url}
"""
    return _title(p, lang), body


# ---------------------------------------------------------------------------
# 状态：发过的、被拒的、跳过的，全部落盘，永不再发
# ---------------------------------------------------------------------------
def load_state(path: Path) -> dict:
    if path.exists():
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            print(f"[!] 状态文件损坏，已备份为 {path.name}.bak")
            path.replace(path.with_suffix(".json.bak"))
    return {"sent": {}, "declined": {}, "skipped": {}}


def save_state(path: Path, state: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(state, ensure_ascii=False, indent=2), encoding="utf-8")
    tmp.replace(path)


def contacted_today(state: dict) -> int:
    today = datetime.now().astimezone().date().isoformat()
    return sum(1 for v in (state.get("sent") or {}).values()
               if str(v.get("sent_at", ""))[:10] == today)


def skip_reason(p: Project, state: dict) -> str:
    repo = p.full_name
    owner = p.owner.lower()
    sent = state.get("sent") or {}
    if repo in sent:
        return f"已于 {str(sent[repo].get('sent_at', ''))[:10]} 联系过"
    if repo in (state.get("declined") or {}):
        return "对方表示不希望再收到"
    for name, rec in sent.items():
        if name.split("/")[0].lower() == owner:
            return f"同作者已有仓库联系过（{name}）"
    return ""


# ---------------------------------------------------------------------------
# 名单
# ---------------------------------------------------------------------------
def latest_json(out_dir: Path) -> Path | None:
    files = sorted(out_dir.glob("eink-projects-*.json"))
    return files[-1] if files else None


def load_projects(path: Path) -> list[Project]:
    data = json.loads(path.read_text(encoding="utf-8"))
    valid = set(Project.__dataclass_fields__)
    out = []
    for row in data.get("projects") or []:
        out.append(Project(**{k: v for k, v in row.items() if k in valid}))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="给墨水屏开源作者发邀请（默认只出草稿，不发）")
    ap.add_argument("--from-json", help="用哪份筛选结果，默认取 output/eink-scan 下最新一份")
    ap.add_argument("--out", default="output/eink-scan", help="输出目录")
    ap.add_argument("--tiers", nargs="*", default=["A"], help="只处理哪些级别，默认 A")
    ap.add_argument("--limit", type=int, default=12, help="最多准备多少份草稿")
    ap.add_argument("--only", nargs="*", help="只处理这些仓库（owner/name）")
    ap.add_argument("--lang", choices=["auto", "en", "zh"], default="auto")
    ap.add_argument("--skip-commercial", action="store_true", help="跳过疑似同行 / 厂商账号")
    ap.add_argument("--send", action="store_true", help="真的发（不加就是 dry-run）")
    ap.add_argument("--yes", action="store_true", help="不逐条确认（仍受次数上限约束）")
    ap.add_argument("--max-per-run", type=int, default=DEFAULT_MAX_PER_RUN)
    ap.add_argument("--max-per-day", type=int, default=DEFAULT_MAX_PER_DAY)
    ap.add_argument("--delay", type=int, default=DEFAULT_DELAY_S, help="两条之间隔几秒")
    ap.add_argument("--home-url", default=HOME_URL)
    ap.add_argument("--try-url", default=TRY_URL)
    ap.add_argument("--no-link-check", action="store_true", help="跳过链接可达性检查")
    ap.add_argument("--test-repo", help="把一条草稿发到这个仓库，用来验证权限与链路")
    ap.add_argument("--decline", nargs="*", metavar="OWNER/REPO",
                    help="把仓库记为「对方不希望再收到」，以后永久跳过")
    args = ap.parse_args()

    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    me = viewer_login()
    if not me:
        print("无法读取 GitHub 身份，先跑 gh auth login")
        return 2

    out_dir = Path(args.out)
    state_path = out_dir / "outreach-state.json"
    drafts_dir = out_dir / "invites"
    drafts_dir.mkdir(parents=True, exist_ok=True)
    state = load_state(state_path)

    # 有人说「不用了」就立刻记下来——这条规则必须是硬的，否则下一轮脚本又会找上他。
    if args.decline:
        for repo in args.decline:
            state.setdefault("declined", {})[repo] = datetime.now().astimezone().isoformat(timespec="seconds")
        save_state(state_path, state)
        print(f"已标记 {len(args.decline)} 个仓库为不再联系：{', '.join(args.decline)}")
        return 0

    # -- 名单 ---------------------------------------------------------------
    if args.test_repo:
        # 只走「发送」这一段，用来验证 token 和链路，不碰真实名单
        title, body = build_draft(
            Project(full_name=args.test_repo, caps={"data": True},
                    evidence={"data": "scripts/render.py"}),
            args.lang if args.lang != "auto" else "en", me, args.try_url, args.home_url)
        return send_one(args.test_repo, title, body, me, state, state_path,
                        drafts_dir, args, dry_run=not args.send)

    if args.from_json:
        src = Path(args.from_json)
    else:
        src = latest_json(out_dir)
    if not src or not src.exists():
        print(f"没有找到筛选结果。先跑：python scripts/find_eink_projects.py")
        return 2
    print(f"→ 名单来源 {src}")

    projects = load_projects(src)
    pool = [p for p in projects if p.full_name in set(args.only)] if args.only else \
        [p for p in projects if p.tier in set(args.tiers)]
    pool.sort(key=lambda p: (-p.score, -p.stars))
    print(f"→ {len(pool)} 个候选（tier={'/'.join(args.tiers)}）")
    if args.skip_commercial:
        before = len(pool)
        pool = [p for p in pool if not commercial_hint(p)]
        print(f"→ 去掉疑似同行 / 厂商账号 {before - len(pool)} 个，剩 {len(pool)} 个")

    # -- 本地去重 -----------------------------------------------------------
    fresh: list[Project] = []
    for p in pool:
        reason = skip_reason(p, state)
        if reason:
            print(f"  · 跳过 {p.full_name}：{reason}")
            continue
        fresh.append(p)
    print(f"→ 去重后 {len(fresh)} 个")

    # -- 资格核查 -----------------------------------------------------------
    metas = fetch_meta([p.full_name for p in fresh])
    eligible: list[Project] = []
    for p in fresh:
        meta = metas.get(p.full_name) or Meta(full_name=p.full_name, blocker="未取到仓库信息")
        if not meta.eligible:
            print(f"  · 跳过 {p.full_name}：{meta.blocker}")
            state.setdefault("skipped", {})[p.full_name] = {
                "at": datetime.now().isoformat(timespec="seconds"), "reason": meta.blocker}
            continue
        p_meta[meta.full_name] = meta
        eligible.append(p)
    save_state(state_path, state)
    print(f"→ 资格核查后 {len(eligible)} 个")

    # -- 证据扫描已由 find_eink_projects 做，这里只补 issue 模板 -----------------
    targets = eligible[: args.limit]
    for p in targets:
        meta = p_meta.get(p.full_name)
        if meta:
            meta.has_template = has_issue_template(p.full_name)
        time.sleep(0.2)

    # -- 出草稿 -------------------------------------------------------------
    plan: list[dict] = []
    for p in targets:
        lang = _pick_lang(p, args.lang)
        title, body = build_draft(p, lang, me, args.try_url, args.home_url)
        slug = p.full_name.replace("/", "__")
        path = drafts_dir / f"{slug}.md"
        path.write_text(
            f"<!-- 目标：{p.full_name}　作者：{p.owner}　tier={p.tier} score={p.score} -->\n"
            f"<!-- 标题：{title} -->\n"
            f"<!-- 建议：{('、'.join(p.reasons[:4]) or '—')} -->\n\n{body}",
            encoding="utf-8")
        plan.append({"repo": p.full_name, "owner": p.owner, "tier": p.tier,
                     "score": p.score, "lang": lang, "title": title, "file": str(path),
                     "template": bool(p_meta.get(p.full_name) and p_meta[p.full_name].has_template),
                     "discussions": bool(p_meta.get(p.full_name) and p_meta[p.full_name].discussions),
                     "commercial": commercial_hint(p)})

    (drafts_dir / "plan.md").write_text(_plan_md(plan), encoding="utf-8")

    print(f"\n→ 草稿 {len(plan)} 份，写在 {drafts_dir}")
    for row in plan:
        flags = []
        if row["commercial"]:
            flags.append(row["commercial"])
        if row["template"]:
            flags.append("有 issue 模板")
        if row["discussions"]:
            flags.append("开了 Discussions")
        print(f"  · {row['repo']:<42} {row['lang']}  {' '.join(flags)}")

    if not args.send:
        print("\n这是 dry-run，什么都没发。")
        print("  · 先读一遍草稿，把开场那句话换成你自己的话（别原样群发）")
        print(f"  · 确认无误：python scripts/invite_eink_authors.py --send --max-per-run {args.max_per_run}")
        print(f"  · 只想发一个：python scripts/invite_eink_authors.py --send --only {plan[0]['repo'] if plan else 'owner/name'}")
        return 0

    # -- 发送 ---------------------------------------------------------------
    if not args.no_link_check:
        for label, url in (("主页", args.home_url), ("试出图页", args.try_url)):
            code = http_status(url)
            if code != 200:
                print(f"\n[!] {label} {url} 返回 {code}，别把这种链接贴到别人仓库里。")
                print("    上线后再发，或确认地址是否变了（--home-url / --try-url）。")
                return 3
        print("→ 链接可达，继续")

    budget = min(args.max_per_run, args.max_per_day - contacted_today(state))
    if budget <= 0:
        print(f"\n今天已达上限（{args.max_per_day} 条）。明天再来，或者 --max-per-day 调高（不建议）。")
        return 0
    print(f"→ 本次预算 {budget} 条（今日已发 {contacted_today(state)}）")

    sent = 0
    for row in plan:
        if sent >= budget:
            print(f"→ 到本轮上限 {budget} 条，其余留到下次")
            break
        repo = row["repo"]
        blocking = already_involved(repo, me)
        if blocking:
            print(f"  · 跳过 {repo}：{blocking}")
            state.setdefault("skipped", {})[repo] = {
                "at": datetime.now().isoformat(timespec="seconds"), "reason": blocking}
            save_state(state_path, state)
            continue
        # 文件头是几行注释，正文从第一个空行之后开始
        body = _strip_header(Path(row["file"]).read_text(encoding="utf-8"))
        if not args.yes:
            print(f"\n{'=' * 70}\n{repo}\n{row['title']}\n{'-' * 70}\n{body}{'=' * 70}")
            # 不用 sys.stdin.isatty() 判断：Windows 上 NUL 设备也会返回 True，
            # 定时任务 / 管道里跑就会卡在 input()。直接接 EOF 更可靠。
            try:
                ans = input("发这一条？[y/N/q] ").strip().lower()
            except (EOFError, KeyboardInterrupt):
                print("\n[!] 没有交互终端，需要 --yes 才会真发。已停止（什么都没发）。")
                return 4
            if ans == "q":
                print("已停止")
                break
            if ans != "y":
                print("  跳过")
                continue
        if send_one(repo, row["title"], body, me, state, state_path, drafts_dir,
                    args, dry_run=False) == 0:
            sent += 1
            if sent < budget:
                print(f"  …等 {args.delay}s")
                time.sleep(args.delay)
    print(f"\n完成：本次发出 {sent} 条，累计今天 {contacted_today(state)} 条")
    return 0


def _strip_header(text: str) -> str:
    lines = text.splitlines()
    while lines and lines[0].startswith("<!--"):
        lines.pop(0)
        while lines and not lines[0].strip():
            lines.pop(0)
    return "\n".join(lines).strip() + "\n"


def send_one(repo: str, title: str, body: str, me: str, state: dict, state_path: Path,
             drafts_dir: Path, args, dry_run: bool) -> int:
    """真发一条，并把结果记进状态文件。返回 0 成功 / 1 失败。"""
    body_file = drafts_dir / f"{repo.replace('/', '__')}.send.md"
    body_file.write_text(body, encoding="utf-8")
    if dry_run:
        print(f"[dry-run] 会发到 {repo}：{title}")
        print(f"          正文 {len(body)} 字，见 {body_file}")
        return 0
    try:
        url = run_gh(["issue", "create", "--repo", repo, "--title", title,
                      "--body-file", str(body_file)])
    except Exception as exc:  # noqa: BLE001
        print(f"  [!] {repo} 发送失败：{exc}")
        return 1
    state.setdefault("sent", {})[repo] = {
        "sent_at": datetime.now().astimezone().isoformat(timespec="seconds"),
        "owner": repo.split("/")[0],
        "issue": url,
        "title": title,
    }
    save_state(state_path, state)
    log = Path(args.out) / "outreach-log.jsonl"
    with log.open("a", encoding="utf-8") as fh:
        fh.write(json.dumps({"at": state["sent"][repo]["sent_at"], "repo": repo,
                             "issue": url, "title": title, "by": me},
                            ensure_ascii=False) + "\n")
    print(f"  ✓ {repo} → {url}")
    return 0


def _plan_md(plan: list[dict]) -> str:
    lines = ["# 本轮邀请草稿\n",
             f"生成于 {datetime.now().strftime('%Y-%m-%d %H:%M')}，共 {len(plan)} 条。\n",
             "> 草稿是机器按他自己的证据拼的，**开场那句必须改成你自己的话**。",
             "> 原样群发是最容易被识破的方式。\n",
             "| # | 仓库 | 级别 | 语言 | 注意 | 草稿 |",
             "| --- | --- | --- | --- | --- | --- |"]
    for i, row in enumerate(plan, 1):
        notes = []
        if row.get("commercial"):
            notes.append(row["commercial"])
        if row["template"]:
            notes.append("有 issue 模板，可能被 bot 关")
        if row["discussions"]:
            notes.append("开了 Discussions，可改发 Discussion")
        lines.append(f"| {i} | [{row['repo']}](https://github.com/{row['repo']}) | {row['tier']} | "
                     f"{row['lang']} | {'；'.join(notes) or '—'} | `{Path(row['file']).name}` |")
    lines.append("\n## 发送顺序建议\n")
    lines.append("先发 1 条，等 48 小时看反应。有人回、有人 star、有人提 issue，说明话术成立，再发下一条。\n")
    lines.append("没有人回就换开场，不要加速——同一天发 5 条相同模板，等于自己举报自己。\n")
    return "\n".join(lines)


if __name__ == "__main__":
    raise SystemExit(main())
