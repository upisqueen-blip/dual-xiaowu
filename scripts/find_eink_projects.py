#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""墨水屏 / 电子纸开源项目筛选器 —— 给「小悟众创」找值得邀请的 GitHub 作者。

筛选口径（四条硬门槛 + 三条能力信号）：

    硬门槛
    1. 近 N 天（默认 180）有「人」提交过代码 —— 排除只有 dependabot / CI 在动的仓库
    2. 许可证明确且在白名单内 —— MIT / Apache-2.0 / GPL 系（默认口径），没有 LICENSE 的一律不进
    3. 不是 fork、不是归档、不是我们自己的仓库
    4. 近 N 天有过 issue 或 PR 活动（能看出作者还在维护）

    能力信号（至少命中 1 项，命中越多分越高）
    A. 驱动   —— 仓库里有 EPD / GxEPD2 / SSD16xx / UC81xx / IL03xx 之类屏驱动代码
    B. 数据   —— 有 HTTP 拉取、API、JSON 解析：接进来能直接喂真数据
    C. 出图   —— 有位图 / PIL / drawBitmap 之类的绘图或图片渲染能力

用法::

    python scripts/find_eink_projects.py                    # 默认 180 天，输出到 output/eink-scan/
    python scripts/find_eink_projects.py --days 90 --top 40
    python scripts/find_eink_projects.py --licenses MIT Apache-2.0
    python scripts/find_eink_projects.py --no-capability-scan   # 只跑元数据，快很多

依赖：本机 `gh` 已登录（`gh auth login`），或环境变量 GITHUB_TOKEN。不需要装任何 pip 包。
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from pathlib import Path

# ---------------------------------------------------------------------------
# 搜索词：中英文都跑一遍。GitHub 仓库搜索支持 in:name,description,readme，
# 只搜 topic 会漏掉大量没打 tag 的老仓库。
# ---------------------------------------------------------------------------
DEFAULT_QUERIES = [
    "epaper in:name,description,readme",
    "e-ink in:name,description,readme",
    "eink in:name,description,readme",
    "gxepd in:name,description,readme",
    "waveshare epaper in:name,description,readme",
    "inkplate in:name,description,readme",
    "epd in:name,description",
    "topic:epaper",
    "topic:e-ink",
    "topic:eink",
    "墨水屏 in:name,description,readme",
    "电子墨水 in:name,description,readme",
]

# 默认许可白名单：只要明确写了这三类，后续无论是直接引用、改一版分发，
# 还是只在自家平台跑，都不至于踩到「公开≠开源」的坑。
PRIMARY_LICENSES = {"MIT", "Apache-2.0", "GPL-2.0", "GPL-3.0"}
# 同族补充（用户点名外常遇到的 copyleft / 宽松许可，默认也放行，可用 --strict 关掉）
EXTRA_LICENSES = {"LGPL-2.1", "LGPL-3.0", "AGPL-3.0", "BSD-2-Clause", "BSD-3-Clause", "MPL-2.0"}

BOT_LOGIN_RE = re.compile(r"(\[bot\]$|^(dependabot|renovate|github-actions|pre-commit-ci|"
                          r"imgbot|allcontributors|codecov|snyk-bot))", re.I)

CAP_LABEL = {"driver": "懂屏驱动", "data": "有 HTTP 数据源", "render": "能出图"}

DRIVER_RE = re.compile(
    r"(gxepd|epd_?if|ssd16\d\d|uc81\d\d|il03\d\d|il91874|waveshare|inkplate|"
    r"display_?driver|epd\d|\bEPD\b)", re.I)
# 数据能力只看「真的有在拉 HTTP」：README 里的普通网址一律不算（几乎每个 README 都有链接）。
HTTP_RE = re.compile(
    r"(httpclient|wificlient|esp_http_client|httpx|requests\.(get|post)|urllib|aiohttp|axios|"
    r"fetch\(|https?\.get|/api/|api[_-]?key|json\.load|json\.parse|graphql)", re.I)
RENDER_RE = re.compile(
    r"(drawbitmap|drawimage|imagedraw|imagefont|\bPIL\b|pillow|framebuffer|cbuffer|"
    r"drawtext|draw_text|render_|_render|gfx_)", re.I)
EMBED_RE = re.compile(r"(^|/)(platformio\.ini|idf_component\.yml|CMakeLists\.txt|sdkconfig)$|\.ino$", re.I)


# ---------------------------------------------------------------------------
# GitHub 访问层：优先走 gh CLI（复用登录态），没有就退回 GITHUB_TOKEN + urllib
# ---------------------------------------------------------------------------
class GitHub:
    def __init__(self, verbose: bool = True) -> None:
        self.verbose = verbose
        self._has_gh = self._check_gh()
        self._token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
        if not self._has_gh and not self._token:
            raise SystemExit("需要 gh CLI（gh auth login）或环境变量 GITHUB_TOKEN")

    def _check_gh(self) -> bool:
        try:
            out = subprocess.run(["gh", "auth", "status"], capture_output=True, text=True, timeout=20)
            return out.returncode == 0
        except (FileNotFoundError, subprocess.TimeoutExpired):
            return False

    # -- 底层 ---------------------------------------------------------------
    def _gh(self, args: list[str], payload: str | None = None) -> dict:
        proc = subprocess.run(["gh", *args], input=payload, capture_output=True,
                              text=True, encoding="utf-8", timeout=180)
        # GraphQL 在部分 alias 解析失败时（仓库改名 / 删库）也会给非 0 退出码，
        # 但 stdout 里仍带着其余仓库的数据。能解析就用，不要整批丢掉。
        if proc.stdout.strip():
            try:
                return json.loads(proc.stdout)
            except json.JSONDecodeError:
                pass
        if proc.returncode != 0:
            raise RuntimeError(f"gh {' '.join(args[:3])} 失败: {proc.stderr.strip()[:300]}")
        return {}

    def _http(self, path: str) -> dict:
        url = path if path.startswith("http") else f"https://api.github.com{path}"
        req = urllib.request.Request(url, headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {self._token}",
            "User-Agent": "xiaowu-eink-scan/1.0",
        })
        for attempt in range(3):
            try:
                with urllib.request.urlopen(req, timeout=30) as resp:
                    return json.loads(resp.read().decode("utf-8"))
            except urllib.error.HTTPError as exc:
                if exc.code in (403, 429) and attempt < 2:
                    wait = 30 * (attempt + 1)
                    self._log(f"  触发限流，等待 {wait}s 后重试…")
                    time.sleep(wait)
                    continue
                raise RuntimeError(f"HTTP {exc.code} {path}") from exc
        return {}

    # -- 能力 ---------------------------------------------------------------
    def search_repos(self, query: str, days: int, pages: int = 1) -> list[dict]:
        since = (datetime.now(timezone.utc) - timedelta(days=days)).strftime("%Y-%m-%d")
        q = urllib.parse.quote(f"{query} pushed:>{since}", safe="")
        items: list[dict] = []
        for page in range(1, pages + 1):
            path = f"search/repositories?q={q}&sort=updated&order=desc&per_page=100&page={page}"
            data = self._gh(["api", path]) if self._has_gh else self._http(f"/{path}")
            batch = data.get("items") or []
            items.extend(batch)
            if len(batch) < 100:
                break
        return items

    def graphql(self, query: str) -> dict:
        if self._has_gh:
            return self._gh(["api", "graphql", "--input", "-"], payload=json.dumps({"query": query}))
        body = json.dumps({"query": query}).encode()
        req = urllib.request.Request("https://api.github.com/graphql", data=body, headers={
            "Authorization": f"Bearer {self._token}",
            "Content-Type": "application/json",
            "User-Agent": "xiaowu-eink-scan/1.0",
        })
        with urllib.request.urlopen(req, timeout=60) as resp:
            return json.loads(resp.read().decode("utf-8"))

    def tree(self, full_name: str, branch: str) -> list[str]:
        """递归目录树，只取路径（判断能力信号最有效的一手证据）。"""
        try:
            data = self._gh(["api", f"repos/{full_name}/git/trees/{branch}?recursive=1"]) \
                if self._has_gh else self._http(f"/repos/{full_name}/git/trees/{branch}?recursive=1")
            return [n.get("path", "") for n in (data.get("tree") or []) if n.get("type") == "blob"]
        except Exception:  # noqa: BLE001 - 空仓库 / 超大仓库 / 分支名异常都当无信号
            return []

    def readme(self, full_name: str) -> str:
        try:
            data = self._gh(["api", f"repos/{full_name}/readme"]) if self._has_gh \
                else self._http(f"/repos/{full_name}/readme")
            raw = data.get("content") or ""
            import base64
            return base64.b64decode(raw).decode("utf-8", "ignore")[:20000]
        except Exception:  # noqa: BLE001
            return ""

    def _log(self, msg: str) -> None:
        if self.verbose:
            print(msg, flush=True)


# ---------------------------------------------------------------------------
# 数据模型
# ---------------------------------------------------------------------------
@dataclass
class Project:
    full_name: str
    url: str = ""
    description: str = ""
    stars: int = 0
    forks: int = 0
    language: str = ""
    license: str = ""
    pushed_at: str = ""
    created_at: str = ""
    topics: list[str] = field(default_factory=list)
    default_branch: str = ""
    open_issues: int = 0

    last_commit_at: str = ""
    last_commit_author: str = ""
    bot_only: bool = False
    commits_6m: int = 0
    issues_active_6m: int = 0
    issues_answered_6m: int = 0
    pr_active_6m: int = 0

    caps: dict[str, bool] = field(default_factory=dict)
    evidence: dict[str, str] = field(default_factory=dict)
    embeddable: bool = False

    score: float = 0.0
    tier: str = "C"
    reasons: list[str] = field(default_factory=list)
    excluded: str = ""

    @property
    def owner(self) -> str:
        return self.full_name.split("/")[0]

    @property
    def name(self) -> str:
        return self.full_name.split("/")[-1]

    @property
    def cap_count(self) -> int:
        return sum(1 for v in self.caps.values() if v)


# ---------------------------------------------------------------------------
# 各环节
# ---------------------------------------------------------------------------
def discover(gh: GitHub, queries: list[str], days: int, pages: int) -> dict[str, Project]:
    found: dict[str, Project] = {}
    for idx, query in enumerate(queries, 1):
        try:
            items = gh.search_repos(query, days, pages=pages)
        except Exception as exc:  # noqa: BLE001
            print(f"  [!] 搜索失败 {query!r}: {exc}")
            continue
        fresh = 0
        for raw in items:
            full = raw.get("full_name") or ""
            if not full or full in found:
                continue
            found[full] = Project(
                full_name=full,
                url=raw.get("html_url") or f"https://github.com/{full}",
                description=(raw.get("description") or "").strip(),
                stars=raw.get("stargazers_count") or 0,
                forks=raw.get("forks_count") or 0,
                language=raw.get("language") or "",
                license=((raw.get("license") or {}).get("spdx_id") or ""),
                pushed_at=raw.get("pushed_at") or "",
                created_at=raw.get("created_at") or "",
                topics=raw.get("topics") or [],
                default_branch=raw.get("default_branch") or "",
                open_issues=raw.get("open_issues_count") or 0,
            )
            if raw.get("fork"):
                found[full].excluded = "fork"
            elif raw.get("archived"):
                found[full].excluded = "archived"
            fresh += 1
        print(f"  [{idx}/{len(queries)}] {query!r} → {len(items)} 条，新增 {fresh}，累计 {len(found)}")
        time.sleep(2.2)  # 搜索接口 30 次/分钟，留余量
    return found


def _fragment(alias: str, p: Project, since_iso: str) -> str:
    owner = re.sub(r"[^A-Za-z0-9._-]", "", p.owner)
    name = re.sub(r"[^A-Za-z0-9._-]", "", p.name)
    return (
        f'{alias}: repository(owner: "{owner}", name: "{name}") {{'
        f' nameWithOwner'
        f' defaultBranchRef {{ target {{ ... on Commit {{'
        f'  committedDate'
        f'  recent: history(first: 50, since: "{since_iso}") {{ totalCount }}'
        f'  lastCommit: history(first: 6) {{ nodes {{ committedDate author {{ user {{ login }} name }} }} }}'
        f' }} }} }}'
        # 注意是 first 不是 last：按 UPDATED_AT DESC 排序后再取 last，拿到的是
        # 「最久没动」的那几条，会把还在维护的仓库判成死仓库。
        f' recentIssues: issues(first: 30, orderBy: {{field: UPDATED_AT, direction: DESC}}) {{'
        f'  nodes {{ updatedAt comments {{ totalCount }} }} }}'
        f' recentPRs: pullRequests(first: 10, orderBy: {{field: UPDATED_AT, direction: DESC}}) {{'
        f'  nodes {{ updatedAt }} }}'
        f' }}'
    )


def _apply(p: Project, node: dict, cutoff: datetime) -> None:
    branch = (node.get("defaultBranchRef") or {}).get("target") or {}
    hist = branch.get("recent") or {}
    p.commits_6m = int(hist.get("totalCount") or 0)
    # 最近 6 条里找第一条「人」的提交：dependabot / CI 也会推 commit，
    # 只看最后一条会把已经没人管、只剩自动更新的仓库算成「活跃」。
    nodes = (branch.get("lastCommit") or {}).get("nodes") or []
    human = None
    for commit in nodes:
        who = commit.get("author") or {}
        login = (who.get("user") or {}).get("login") or who.get("name") or ""
        if login and BOT_LOGIN_RE.search(login):
            continue
        human = commit
        p.last_commit_author = login
        break
    if human is None and nodes:
        p.bot_only = True
        p.last_commit_author = "（仅机器人）"
    p.last_commit_at = (human or (nodes[0] if nodes else {})).get("committedDate") \
        or branch.get("committedDate") or ""
    for issue in (node.get("recentIssues") or {}).get("nodes") or []:
        updated = _parse_iso(issue.get("updatedAt"))
        if updated and updated >= cutoff:
            p.issues_active_6m += 1
            if int(((issue.get("comments") or {}).get("totalCount")) or 0) > 0:
                p.issues_answered_6m += 1
    for pr in (node.get("recentPRs") or {}).get("nodes") or []:
        updated = _parse_iso(pr.get("updatedAt"))
        if updated and updated >= cutoff:
            p.pr_active_6m += 1


def enrich(gh: GitHub, projects: list[Project], days: int, batch: int = 12) -> None:
    """GraphQL 批量取「近 N 天人类提交 / issue / PR」——判断作者还活着没有。"""
    since_iso = (datetime.now(timezone.utc) - timedelta(days=days)).strftime("%Y-%m-%dT%H:%M:%SZ")
    cutoff = datetime.now(timezone.utc) - timedelta(days=days)
    for start in range(0, len(projects), batch):
        chunk = projects[start:start + batch]
        parts = [_fragment(f"r{i}", p, since_iso) for i, p in enumerate(chunk)]
        data: dict = {}
        try:
            data = gh.graphql("query{" + " ".join(parts) + "}").get("data") or {}
        except Exception as exc:  # noqa: BLE001
            print(f"  [!] 第 {start // batch + 1} 批富集失败：{exc}")
        for i, p in enumerate(chunk):
            node = data.get(f"r{i}")
            if node:
                _apply(p, node, cutoff)
                continue
            # 批量里某个 alias 解析不到（仓库改名 / 转私有）会把整批拖垮，
            # 单独补一次，补不到就让它自然被后面的门槛筛掉。
            try:
                single = gh.graphql("query{" + _fragment("r", p, since_iso) + "}").get("data") or {}
                if single.get("r"):
                    _apply(p, single["r"], cutoff)
            except Exception:  # noqa: BLE001
                p.excluded = p.excluded or "仓库不可访问"
            time.sleep(0.3)
        gh._log(f"  富集 {min(start + batch, len(projects))}/{len(projects)}")
        time.sleep(0.4)


def _parse_iso(text: str | None) -> datetime | None:
    if not text:
        return None
    try:
        return datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError:
        return None


CACHE_VERSION = 5
ENRICH_FIELDS = ("last_commit_at", "last_commit_author", "bot_only", "commits_6m",
                 "issues_active_6m", "issues_answered_6m", "pr_active_6m")
CAP_FIELDS = ("caps", "evidence", "embeddable")


def load_cache(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def save_cache(path: Path, cache: dict) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(cache, ensure_ascii=False), encoding="utf-8")
    except OSError:
        pass


def cache_hit(entry: dict, stage: str, ttl_hours: float) -> bool:
    """同一仓库在 TTL 内重复跑时直接复用，别把 API 配额浪费在重算上。

    规则改了就升 CACHE_VERSION，让旧结论整体失效，避免「跑出来还是老结果」。
    """
    if (entry or {}).get("_v") != CACHE_VERSION:
        return False
    stamp = _parse_iso((entry or {}).get(f"{stage}_at"))
    if not stamp:
        return False
    return (datetime.now(timezone.utc) - stamp).total_seconds() < ttl_hours * 3600


def cache_apply(p: Project, entry: dict, fields: tuple[str, ...]) -> None:
    for name in fields:
        if name in entry:
            value = entry[name]
            setattr(p, name, dict(value) if isinstance(value, dict) else value)


def cache_store(cache: dict, p: Project, stage: str, fields: tuple[str, ...]) -> None:
    entry = cache.setdefault(p.full_name, {})
    entry["_v"] = CACHE_VERSION
    for name in fields:
        entry[name] = getattr(p, name)
    entry[f"{stage}_at"] = datetime.now(timezone.utc).isoformat(timespec="seconds")


CODE_EXT = r"(c|cc|cpp|cxx|h|hpp|py|ino|js|ts|jsx|tsx|go|rs|kt|java|swift|lua)"
# 相关性：仓库名 / 简介 / topics 里得真的是墨水屏，或者仓库里有屏驱动代码。
# 有些项目只是在 README 里列了一串硬件型号，搜得到但跟屏没关系，得排掉。
RELEVANT_RE = re.compile(r"(e-?paper|e-?ink|epd|inkplate|gxepd|墨水屏|电子墨水|epaper)", re.I)


def scan_capability(gh: GitHub, projects: list[Project]) -> None:
    """读目录树 + README，判断 A 驱动 / B 数据 / C 出图 三项能力，并留证据。

    只认「代码文件」和「README 里的实际用法」，不认仓库里的普通图片 / JSON 配置，
    也不认 README 里的裸链接——否则每个 README 都能命中，等于没筛。
    """
    driver_path = re.compile(
        rf"(^|/)[^/]*(epd|gxepd|ssd16\d\d|uc81\d\d|il03\d\d|epdif|waveshare|inkplate)[^/]*\.{CODE_EXT}$", re.I)
    data_path = re.compile(
        rf"(^|/)(api|apis)/|(^|/)[^/]*(http|fetch|client|sync|net|server|request)[^/]*\.{CODE_EXT}$", re.I)
    render_path = re.compile(
        rf"(^|/)[^/]*(image|img|draw|render|gfx|canvas|painter)[^/]*\.{CODE_EXT}$", re.I)

    for idx, p in enumerate(projects, 1):
        paths = gh.tree(p.full_name, p.default_branch or "HEAD")
        readme = gh.readme(p.full_name)

        def first(rx: re.Pattern, pool: list[str]) -> str:
            for item in pool:
                if rx.search(item):
                    return item
            return ""

        driver_hit = first(driver_path, paths)
        data_hit = first(data_path, paths)
        render_hit = first(render_path, paths)

        # 只认「仓库里真的有这类代码文件」。README 里提到 Waveshare / HTTP 太容易
        # 命中（几乎每个相关仓库都会在文档里列一遍硬件型号），拿它当能力会把
        # 「只在 README 里提过墨水屏」的项目也放进来，名单就没法用了。
        p.caps = {"driver": bool(driver_hit), "data": bool(data_hit), "render": bool(render_hit)}
        p.evidence = {
            "driver": driver_hit or _readme_evidence(readme, DRIVER_RE),
            "data": data_hit or _readme_evidence(readme, HTTP_RE),
            "render": render_hit or _readme_evidence(readme, RENDER_RE),
        }
        p.embeddable = any(EMBED_RE.search(x) for x in paths)
        if idx % 10 == 0:
            gh._log(f"  能力扫描 {idx}/{len(projects)}")
        time.sleep(0.15)


def _readme_evidence(text: str, rx: re.Pattern) -> str:
    m = rx.search(text or "")
    if not m:
        return ""
    start = max(0, m.start() - 24)
    frag = " ".join(text[start:m.end() + 36].split())[:80]
    return f"README: …{frag}…"


def score(p: Project, primary: set[str], extra: set[str], days: int = 180) -> None:
    """打分口径（总分 ~100，故意不给任何一项满分空间，避免一堆 100 分并列）：

    活跃 40（最近提交 25 + 窗口内提交量 10 + PR 5）
    互动 10（issue 有人回复）
    影响力 12（stars，log 压缩）
    许可 8
    能力 24（驱动 / 数据 / 出图 各 8）
    可移植 4（PlatformIO / Arduino / ESP-IDF 工程）
    """
    now = datetime.now(timezone.utc)
    pts = 0.0
    reasons: list[str] = []

    last = _parse_iso(p.last_commit_at)
    if last:
        gap = max(0, (now - last).days)          # 别叫 days：会把窗口参数覆盖掉
        pts += 25.0 * max(0.0, 1 - gap / max(1, days))
        reasons.append(f"{gap} 天前还有人提交")
    if p.commits_6m:
        pts += min(10.0, 3.0 * math.log1p(p.commits_6m))
        reasons.append(f"近 {days} 天 {p.commits_6m} 次提交")
    if p.pr_active_6m:
        pts += min(5.0, 1.8 * p.pr_active_6m)
        reasons.append(f"近 {days} 天 {p.pr_active_6m} 个 PR 在动")
    if p.issues_answered_6m:
        pts += min(10.0, 3.0 + 1.8 * p.issues_answered_6m)
        reasons.append(f"近 {days} 天 {p.issues_answered_6m} 个 issue 有回复")
    elif p.issues_active_6m:
        pts += 4.0
        reasons.append(f"近 {days} 天 {p.issues_active_6m} 个 issue 有动静")
    if p.stars:
        pts += min(12.0, 4.0 * math.log10(1 + p.stars))
    if p.license in primary:
        pts += 8.0
        reasons.append(f"{p.license} 许可，合作口径清楚")
    elif p.license in extra:
        pts += 5.0
        reasons.append(f"{p.license} 许可（copyleft，注意分发方式）")
    pts += 8.0 * p.cap_count
    for key, ok in p.caps.items():
        if ok:
            reasons.append(CAP_LABEL[key])
    if p.embeddable:
        pts += 4.0
        reasons.append("工程能编译进嵌入式（PlatformIO / Arduino / ESP-IDF）")

    p.score = round(min(100.0, pts), 1)
    p.reasons = reasons


def assign_tiers(rows: list[Project]) -> None:
    """分级按「本轮排名」而不是绝对分。

    绝对分只说明「够不够格进候选」，进不来的人早在门槛那一步就被筛掉了；
    真正要回答的是「这周先联系谁」，所以 A 取前 30%，B 取 30%~70%，其余 C。
    """
    total = max(1, len(rows))
    for rank, p in enumerate(rows):
        pct = rank / total
        if pct < 0.3 and p.cap_count >= 2:
            p.tier = "A"
        elif pct < 0.7 and p.cap_count >= 1:
            p.tier = "B"
        else:
            p.tier = "C"


def gate(p: Project, days: int, allowed: set[str], exclude_owners: set[str]) -> str:
    """返回空串表示通过；否则返回排除原因。"""
    if p.excluded:
        return p.excluded
    if p.owner.lower() in exclude_owners:
        return "自家仓库"
    if p.license not in allowed:
        return f"许可证 {p.license or '缺失'}"
    last = _parse_iso(p.last_commit_at)
    if not last:
        return "读不到提交记录"
    if (datetime.now(timezone.utc) - last).days > days:
        return f"最后一次提交在 {days} 天前以外"
    if p.commits_6m <= 0:
        return f"近 {days} 天没有提交"
    if p.bot_only:
        return f"近 {days} 天只有机器人在提交（dependabot / CI）"
    return ""


# ---------------------------------------------------------------------------
# 输出
# ---------------------------------------------------------------------------
def write_csv(path: Path, rows: list[Project]) -> None:
    cols = [
        ("排名", lambda i, p: i), ("仓库", lambda i, p: p.full_name), ("地址", lambda i, p: p.url),
        ("分级", lambda i, p: p.tier), ("评分", lambda i, p: p.score), ("Stars", lambda i, p: p.stars),
        ("语言", lambda i, p: p.language), ("许可", lambda i, p: p.license),
        ("最后人类提交", lambda i, p: (p.last_commit_at or "")[:10]), ("窗口内提交", lambda i, p: p.commits_6m),
        ("窗口内有回复issue", lambda i, p: p.issues_answered_6m), ("窗口内PR", lambda i, p: p.pr_active_6m),
        ("驱动", lambda i, p: "Y" if p.caps.get("driver") else ""),
        ("数据", lambda i, p: "Y" if p.caps.get("data") else ""),
        ("出图", lambda i, p: "Y" if p.caps.get("render") else ""),
        ("简介", lambda i, p: p.description), ("入选理由", lambda i, p: " / ".join(p.reasons)),
        ("证据-驱动", lambda i, p: p.evidence.get("driver", "")),
        ("证据-数据", lambda i, p: p.evidence.get("data", "")),
        ("证据-出图", lambda i, p: p.evidence.get("render", "")),
        ("Topics", lambda i, p: ", ".join(p.topics[:8])),
    ]
    with path.open("w", encoding="utf-8-sig", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow([c[0] for c in cols])
        for i, p in enumerate(rows, 1):
            writer.writerow([c[1](i, p) for c in cols])


def write_markdown(path: Path, rows: list[Project], stats: dict, args, skipped: list[Project]) -> None:
    lines: list[str] = []
    lines.append("# 墨水屏开源项目 · 可邀请名单\n")
    lines.append(f"> 生成时间：{datetime.now().strftime('%Y-%m-%d %H:%M')}　"
                 f"窗口：近 {args.days} 天　"
                 f"许可口径：{'/'.join(sorted(args.license_set))}\n")
    lines.append("## 一轮筛完的漏斗\n")
    lines.append("| 环节 | 数量 |\n| --- | --- |")
    for label, value in stats.items():
        lines.append(f"| {label} | {value} |")
    lines.append("")
    lines.append("> 分级口径：**A = 本轮前 30%**（这周先联系这批），B = 30%~70%，C = 其余。"
                 "分数只用于排序，门槛在上一张表里已经卡过了。\n")
    a_list = [p for p in rows if p.tier == "A"]
    lines.append(f"## A 级优先联系（{len(a_list)} 个）\n")
    if not a_list:
        lines.append("_本轮没有跑出 A 级，可放宽 `--days` 或 `--top`。_\n")
    for i, p in enumerate(a_list, 1):
        caps = "、".join(CAP_LABEL[k] for k, v in p.caps.items() if v)
        lines.append(f"### {i}. [{p.full_name}]({p.url})　★{p.stars}　{p.license}")
        lines.append(f"- 一句话：{p.description or '（无简介）'}")
        lines.append(f"- 为什么是他：{'；'.join(p.reasons)}")
        lines.append(f"- 能力证据：{caps}")
        for key, label in [("driver", "驱动"), ("data", "数据"), ("render", "出图")]:
            if p.evidence.get(key):
                lines.append(f"  - {label}：`{p.evidence[key][:80]}`")
        lines.append(f"- 建议开场：{outreach_line(p)}")
        lines.append("")

    lines.append("## 完整排序\n")
    lines.append("| # | 仓库 | 分 | 级 | ★ | 许可 | 最后人类提交 | 提交 | 回复 | 能力 |")
    lines.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for i, p in enumerate(rows, 1):
        caps = "".join([("驱" if p.caps.get("driver") else "·"),
                        ("数" if p.caps.get("data") else "·"),
                        ("图" if p.caps.get("render") else "·")])
        lines.append(f"| {i} | [{p.full_name}]({p.url}) | {p.score} | {p.tier} | {p.stars} | "
                     f"{p.license} | {(p.last_commit_at or '')[:10]} | {p.commits_6m} | "
                     f"{p.issues_answered_6m} | {caps} |")
    lines.append("")

    if skipped:
        lines.append("<details><summary>被排除的仓库（前 40 条）</summary>\n")
        lines.append("| 仓库 | 原因 |\n| --- | --- |")
        for p in skipped[:40]:
            lines.append(f"| {p.full_name} | {p.excluded} |")
        lines.append("\n</details>\n")

    lines.append("## 后续动作建议\n")
    lines.append("1. 先发 PR / issue，不要一上来谈合作 —— 让对方在自己的仓库里看到真实改动。")
    lines.append("2. PR 描述里只放一条身份链接（项目主页），别堆营销文案。")
    lines.append("3. 对方回应后，再邀请到众创平台：注册 → 导入仓库 → 生成设备出图 → 上架。")
    lines.append("4. 上架不等于买断：作者保留自己仓库的著作权，平台只承载「设备端呈现」。")
    path.write_text("\n".join(lines), encoding="utf-8")


def outreach_line(p: Project) -> str:
    """给一条不像群发模板的开场，最多两个具体点。"""
    clauses = []
    if p.caps.get("driver"):
        clauses.append("屏驱动那块写得干净")
    if p.caps.get("data"):
        clauses.append("取数和渲染是分开的")
    if p.caps.get("render"):
        clauses.append("出图逻辑独立、移植成本低")
    if not clauses:
        clauses.append("结构挺清楚")
    head = "，".join(clauses[:2])
    return (f"「{p.name}」{head}。我们在做一块 4.2 寸双屏墨水屏（A 屏黑白 / B 屏三色），"
            f"照着你的做法接了一版，改动都收在一个分支里，想请你看看值不值得合回去。")


def main() -> int:
    ap = argparse.ArgumentParser(description="筛选值得邀请的墨水屏开源项目")
    ap.add_argument("--days", type=int, default=180, help="活跃窗口，默认 180 天")
    ap.add_argument("--top", type=int, default=60, help="报告里最多保留多少个")
    ap.add_argument("--pages", type=int, default=1, help="每个搜索词翻几页（每页 100）")
    ap.add_argument("--queries", nargs="*", help="自定义搜索词，覆盖默认列表")
    ap.add_argument("--licenses", nargs="*", help="自定义许可白名单，如 MIT Apache-2.0")
    ap.add_argument("--strict", action="store_true", help="只认 MIT / Apache-2.0 / GPL，不含 LGPL/BSD/MPL")
    ap.add_argument("--exclude-owner", nargs="*", default=["upisqueen-blip"], help="排除的 GitHub 账号")
    ap.add_argument("--max-scan", type=int, default=120, help="最多对多少个仓库做能力扫描（控制 API 耗时）")
    ap.add_argument("--no-capability-scan", action="store_true", help="跳过目录树/README 扫描")
    ap.add_argument("--cache-ttl-hours", type=float, default=12,
                    help="同一仓库的富集/能力结果复用多久，默认 12 小时")
    ap.add_argument("--out", default="output/eink-scan", help="输出目录")
    args = ap.parse_args()

    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    if args.licenses:
        args.license_set = set(args.licenses)
    elif args.strict:
        args.license_set = set(PRIMARY_LICENSES)
    else:
        args.license_set = PRIMARY_LICENSES | EXTRA_LICENSES

    queries = args.queries or DEFAULT_QUERIES
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    gh = GitHub()
    print(f"→ 搜索 {len(queries)} 组关键词，窗口 {args.days} 天")
    found = discover(gh, queries, args.days, args.pages)

    # 先按元数据粗筛，省下 GraphQL 配额：至少要有许可、不是 fork/归档
    coarse = [p for p in found.values()
              if not p.excluded and p.license in args.license_set]
    print(f"→ 元数据粗筛：{len(found)} → {len(coarse)}（去掉 fork/归档/无许可）")

    # 预排序：不能只按 stars，否则全是几个大仓；也不能只按 pushed_at，否则全是
    # 刚建的空仓库。用「影响力 + 新鲜度」的混合分取前 N 个，把 GraphQL 和
    # 目录树扫描的配额花在更可能成的地方。
    now = datetime.now(timezone.utc)

    def prekey(p: Project) -> float:
        last = _parse_iso(p.pushed_at)
        fresh = 0.0
        if last:
            fresh = max(0.0, 1 - (now - last).days / max(1, args.days * 2))
        return math.log10(1 + p.stars) * 0.6 + fresh

    coarse.sort(key=prekey, reverse=True)
    to_enrich = coarse[: max(args.max_scan, args.top)]
    cache = load_cache(out_dir / "cache.json")

    hit = [p for p in to_enrich
           if cache_hit(cache.get(p.full_name, {}), "enriched", args.cache_ttl_hours)]
    hit_names = {p.full_name for p in hit}
    need = [p for p in to_enrich if p.full_name not in hit_names]
    for p in hit:
        cache_apply(p, cache[p.full_name], ENRICH_FIELDS)
    print(f"→ 活跃度：缓存命中 {len(hit)}，需要请求 {len(need)}")
    if need:
        enrich(gh, need, args.days)
        for p in need:
            cache_store(cache, p, "enriched", ENRICH_FIELDS)
        save_cache(out_dir / "cache.json", cache)

    passed: list[Project] = []
    skipped: list[Project] = []
    for p in to_enrich:
        reason = gate(p, args.days, args.license_set, {o.lower() for o in args.exclude_owner})
        if reason:
            p.excluded = reason
            skipped.append(p)
        else:
            passed.append(p)
    print(f"→ 活跃度门槛：{len(to_enrich)} → {len(passed)}")

    active_count = len(passed)
    if not args.no_capability_scan and passed:
        cap_hit = [p for p in passed
                   if cache_hit(cache.get(p.full_name, {}), "scanned", args.cache_ttl_hours)]
        cap_names = {p.full_name for p in cap_hit}
        cap_need = [p for p in passed if p.full_name not in cap_names]
        for p in cap_hit:
            cache_apply(p, cache[p.full_name], CAP_FIELDS)
        print(f"→ 扫描驱动 / 数据 / 出图能力：缓存命中 {len(cap_hit)}，需要请求 {len(cap_need)}")
        if cap_need:
            scan_capability(gh, cap_need)
            for p in cap_need:
                cache_store(cache, p, "scanned", CAP_FIELDS)
            save_cache(out_dir / "cache.json", cache)
        before = len(passed)
        dropped = [p for p in passed if p.cap_count == 0]
        for p in dropped:
            p.excluded = "没有驱动 / 数据 / 出图能力信号"
            skipped.append(p)
        passed = [p for p in passed if p.cap_count > 0]
        print(f"→ 能力门槛：{before} → {len(passed)}")

        offtopic = [p for p in passed
                    if not p.caps.get("driver")
                    and not RELEVANT_RE.search(" ".join([p.name, p.description, " ".join(p.topics)]))]
        for p in offtopic:
            p.excluded = "与墨水屏关系弱（文档里提到过，但仓库不以此为主）"
        skipped.extend(offtopic)
        off_names = {p.full_name for p in offtopic}
        passed = [p for p in passed if p.full_name not in off_names]
        print(f"→ 相关性门槛：去掉 {len(offtopic)} 个只在文档里提到墨水屏的仓库 → {len(passed)}")

    for p in passed:
        score(p, PRIMARY_LICENSES, EXTRA_LICENSES, args.days)
    passed.sort(key=lambda p: (-p.score, -p.stars))
    assign_tiers(passed)
    rows = passed[: args.top]

    stats = {
        "搜索命中（去重）": len(found),
        "有明确许可、非 fork/归档": len(coarse),
        "进入活跃度核查": len(to_enrich),
        "近 %d 天有人类提交（非仅机器人）" % args.days: active_count,
        "有驱动/数据/出图能力": len(passed),
        "A 级优先联系": len([p for p in rows if p.tier == "A"]),
    }

    stamp = datetime.now().strftime("%Y%m%d")
    md_path = out_dir / f"eink-projects-{stamp}.md"
    csv_path = out_dir / f"eink-projects-{stamp}.csv"
    json_path = out_dir / f"eink-projects-{stamp}.json"
    write_markdown(md_path, rows, stats, args, skipped)
    write_csv(csv_path, rows)
    json_path.write_text(json.dumps({
        "generated_at": datetime.now().isoformat(timespec="seconds"),
        "window_days": args.days,
        "licenses": sorted(args.license_set),
        "stats": stats,
        "projects": [{
            **{k: v for k, v in p.__dict__.items() if k not in ("excluded",)},
        } for p in rows],
    }, ensure_ascii=False, indent=2), encoding="utf-8")

    print("\n完成：")
    print(f"  报告 {md_path}")
    print(f"  表格 {csv_path}")
    print(f"  原始 {json_path}")
    print(f"  A 级 {len([p for p in rows if p.tier == 'A'])} / 入选 {len(rows)}")
    for p in rows[:8]:
        print(f"  · [{p.tier}] {p.score:>5} {p.full_name} ★{p.stars} {p.license}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
