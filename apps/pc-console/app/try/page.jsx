"use client";

// 零注册试出图：不登录、不建应用、不连设备，贴一段契约 JSON 就看到双屏真实出图。
// 目的很单纯——让「要不要接入」这个问题在 30 秒内有答案，而不是先走完注册流程。
// 渲染走后端 /api/try/render，与设备取图共用同一套 PIL 出图代码，不是前端画的示意图。

import Link from "next/link";
import { useCallback, useEffect, useRef, useState } from "react";

const BASE = "";
const ENDPOINT = "/backend/api/try/render";

const SAMPLES = [
  {
    key: "quota",
    name: "接口额度",
    hint: "剩下多少、什么时候到期",
    payload: {
      version: 1,
      title: "API 额度",
      subtitle: "codex pulse",
      items: [
        { label: "剩余额度", value: "$42.10", emphasis: "red" },
        { label: "到期倒计时", value: "6 天" },
        { label: "本月用量", value: "68%", progress: 68 },
      ],
      footer: "更新于 17:08",
      qr: "https://dual.xiaowuai.vip",
    },
  },
  {
    key: "market",
    name: "自选行情",
    hint: "涨跌与仓位，一眼看完",
    payload: {
      version: 1,
      title: "自选三只",
      subtitle: "收盘 15:00",
      items: [
        { label: "沪深 300", value: "+1.24%", emphasis: "red" },
        { label: "中证 500", value: "-0.38%", emphasis: "none" },
        { label: "纳指 ETF", value: "+2.10%", emphasis: "red" },
        { label: "仓位", value: "62%", progress: 62 },
      ],
      footer: "数据来自你的行情源",
    },
  },
  {
    key: "build",
    name: "构建状态",
    hint: "最喜欢放桌上的一块屏",
    payload: {
      version: 1,
      title: "main 分支",
      subtitle: "xiaowu-s3-open-platform",
      items: [
        { label: "最新构建", value: "通过", emphasis: "none" },
        { label: "测试覆盖", value: "82%", progress: 82 },
        { label: "待合并 PR", value: "3 个" },
        { label: "上次部署", value: "12 分钟前" },
      ],
      footer: "轮询间隔 5 分钟",
    },
  },
  {
    key: "study",
    name: "今日待办",
    hint: "家庭 / 学习场景",
    payload: {
      version: 1,
      title: "今天要带",
      subtitle: "三年级二班",
      items: [
        { label: "跳绳", value: "体育课" },
        { label: "数学作业", value: "第 3 页" },
        { label: "水杯", value: "记得灌满" },
      ],
      footer: "家长 08:12 留言",
      refresh_minutes: 30,
    },
  },
];

const FIELDS = [
  ["title", "必填", "≤ 24 字，卡片主标题"],
  ["items", "必填", "1 ~ 6 项，每项 label ≤ 12 字、value ≤ 20 字"],
  ["subtitle", "选填", "≤ 32 字，一般放数据来源"],
  ["footer", "选填", "≤ 40 字，更新时间之类"],
  ["qr", "选填", "http(s) 地址，右下角生成入口二维码"],
  ["refresh_minutes", "选填", "1 ~ 1440，平台据此判断拉取频率"],
];

// 作品墙的兜底：新站上架作品少，用内置场景补齐，最后一格留给「你的作品」。
const FALLBACK_WALL = [
  { kind: "demo", label: "学习 · 便签与课程表", a: "persona=DUAL_A", b: "persona=DUAL_B&colors=4" },
  {
    kind: "demo",
    label: "专注 · 番茄钟",
    a: "aMode=pomodoro&sceneKey=office",
    b: "bMode=pomodoro&sceneKey=office&colors=4",
  },
];

function pretty(obj) {
  return JSON.stringify(obj, null, 2);
}

export default function TryPage() {
  const [text, setText] = useState(pretty(SAMPLES[0].payload));
  const [active, setActive] = useState(SAMPLES[0].key);
  const [result, setResult] = useState(null);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const [jsonBad, setJsonBad] = useState("");
  const [copied, setCopied] = useState("");
  const [showcase, setShowcase] = useState(null);
  const timer = useRef(null);
  const seq = useRef(0);

  const render = useCallback(async (raw) => {
    let payload;
    try {
      payload = JSON.parse(raw);
    } catch (e) {
      setJsonBad(e instanceof Error ? e.message : "JSON 格式错误");
      return;
    }
    setJsonBad("");
    const mySeq = ++seq.current;
    setBusy(true);
    setError("");
    try {
      const res = await fetch(`${BASE}${ENDPOINT}`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      });
      const data = await res.json().catch(() => ({}));
      if (mySeq !== seq.current) return; // 旧请求晚到，丢弃
      if (!res.ok && !data.images) throw new Error(data.error || `渲染失败 HTTP ${res.status}`);
      setResult(data);
    } catch (e) {
      if (mySeq !== seq.current) return;
      setError(e instanceof Error ? e.message : "渲染失败");
    } finally {
      if (mySeq === seq.current) setBusy(false);
    }
  }, []);

  // 输入停下 650ms 后自动出图：改一个数字就能立刻看到屏上的变化。
  useEffect(() => {
    if (timer.current) clearTimeout(timer.current);
    timer.current = setTimeout(() => void render(text), 650);
    return () => timer.current && clearTimeout(timer.current);
  }, [text, render]);

  useEffect(() => {
    let alive = true;
    fetch(`${BASE}/backend/api/public/apps`, { cache: "no-store" })
      .then((r) => (r.ok ? r.json() : { apps: [] }))
      .then((d) => alive && setShowcase(d.apps || []))
      .catch(() => alive && setShowcase([]));
    return () => {
      alive = false;
    };
  }, []);

  function pick(sample) {
    setActive(sample.key);
    setText(pretty(sample.payload));
  }

  function format() {
    try {
      setText(pretty(JSON.parse(text)));
      setJsonBad("");
    } catch (e) {
      setJsonBad(e instanceof Error ? e.message : "JSON 格式错误");
    }
  }

  async function copy(key, value) {
    try {
      await navigator.clipboard.writeText(value);
      setCopied(key);
      setTimeout(() => setCopied(""), 1600);
    } catch {
      setCopied("");
    }
  }

  const verdict = result?.verdict || "";
  const verdictText =
    verdict === "pass" ? "契约全部通过" : verdict === "warn" ? "能上屏，有优化建议" : verdict === "fail" ? "还有字段需要补" : "等待出图";

  // 作品墙：已上架作品在前，不足 3 个用内置场景补齐，最后一格留给「你的作品」。
  const wallItems = [
    ...(showcase || []).map((a) => ({ kind: "app", ...a })),
    ...FALLBACK_WALL,
  ].slice(0, 3);

  return (
    <div className="try-shell">
      <header className="api-top">
        <Link href="/developer" className="api-brand">
          <b>小悟众创</b>
          <span>LIVE RENDER · NO SIGNUP</span>
        </Link>
        <nav>
          <Link href="/developer">开发者中心</Link>
          <Link href="/api-docs">API 文档</Link>
        </nav>
      </header>

      <section className="try-hero">
        <span className="eyebrow">试出图 · 不用注册</span>
        <h1>
          贴一段 JSON，
          <br />
          先看它在墨水屏上长什么样
        </h1>
        <p>
          左边写数据，右边就是设备会显示的画面。同一套出图代码，不是效果图。觉得可以，再决定要不要接进来——
          从看到这一屏到真的推到设备，中间只差一个 HTTP 端点。
        </p>
        <div className="try-badges">
          <span>4.2 寸 400×300</span>
          <span>A 屏 · 黑白</span>
          <span>B 屏 · 三色 / 四色全刷</span>
          <span>同一个端点，两屏共用</span>
        </div>
      </section>

      <section className="try-studio">
        <div className="try-panel try-editor">
          <div className="try-panel-head">
            <b>你的数据</b>
            <div className="try-chips">
              {SAMPLES.map((s) => (
                <button key={s.key} className={active === s.key ? "on" : ""} onClick={() => pick(s)} title={s.hint}>
                  {s.name}
                </button>
              ))}
            </div>
          </div>
          <textarea
            className="try-textarea"
            value={text}
            spellCheck={false}
            onChange={(e) => setText(e.target.value)}
            aria-label="契约 JSON"
          />
          <div className="try-editor-foot">
            <span className={jsonBad ? "try-bad" : "try-dim"}>
              {jsonBad ? `JSON 错误：${jsonBad}` : busy ? "渲染中…" : "改动后自动重画"}
            </span>
            <div>
              <button className="ghost" onClick={format}>
                格式化
              </button>
              <button className="ghost" onClick={() => void render(text)}>
                立即出图
              </button>
            </div>
          </div>
        </div>

        <div className="try-panel try-result">
          <div className="try-panel-head">
            <b>设备上的样子</b>
            <span className={`try-verdict ${verdict || "idle"}`}>{verdictText}</span>
          </div>
          <div className="try-screens">
            <figure>
              <div className="try-bezel">
                {result?.images?.a ? <img src={result.images.a} alt="A 屏出图" /> : <span className="try-blank">A</span>}
              </div>
              <figcaption>A 屏 · 黑白 400×300</figcaption>
            </figure>
            <figure>
              <div className="try-bezel color">
                {result?.images?.b ? <img src={result.images.b} alt="B 屏出图" /> : <span className="try-blank">B</span>}
              </div>
              <figcaption>B 屏 · 三色 / 四色</figcaption>
            </figure>
          </div>
          {error ? <div className="try-error">{error}</div> : null}
          {result?.checks?.length ? (
            <ul className="try-checks">
              {result.checks.map((c) => (
                <li key={c.id} className={c.status}>
                  <i>{c.status === "pass" ? "✓" : c.status === "warn" ? "!" : "×"}</i>
                  <div>
                    <b>{c.label}</b>
                    <span>{c.detail}</span>
                  </div>
                </li>
              ))}
            </ul>
          ) : null}
        </div>
      </section>

      <section className="try-block">
        <div className="try-section-head">
          <div>
            <span className="eyebrow">THE CONTRACT</span>
            <h2>屏上只认这几样东西</h2>
          </div>
          <p>
            平台只做三件事：确认这是谁的数据 → 收下你算好的结果 → 渲染到双屏。取数、算法、账号都留在你自己那边，
            我们不连你的数据库，也不要用户的模型密钥。
          </p>
        </div>
        <div className="try-contract">
          <table className="try-table">
            <thead>
              <tr>
                <th>字段</th>
                <th>要求</th>
                <th>说明</th>
              </tr>
            </thead>
            <tbody>
              {FIELDS.map(([k, need, desc]) => (
                <tr key={k}>
                  <td>
                    <code>{k}</code>
                  </td>
                  <td>
                    <span className={`try-need ${need === "必填" ? "on" : ""}`}>{need}</span>
                  </td>
                  <td>{desc}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <div className="try-snippets">
            <Snippet
              id="python"
              title="有数据，就露一个端点"
              copy={copy}
              copied={copied}
              code={`# 你只要暴露这样一个 GET，平台自己来抓
@app.get("/eink/card")
def card(screen: str = "A"):     # 想双屏不同就判断 screen
    return {
        "version": 1,
        "title": "API 额度",
        "items": [
            {"label": "剩余额度", "value": "$42.10", "emphasis": "red"},
            {"label": "本月用量", "value": "68%", "progress": 68},
        ],
        "footer": "更新于 17:08",
    }`}
            />
            <Snippet
              id="curl"
              title="先在这里试一下"
              copy={copy}
              copied={copied}
              code={`curl -X POST https://dual.xiaowuai.vip${ENDPOINT} \\
  -H 'Content-Type: application/json' \\
  -d @card.json        # card.json 就是左边那段

# 返回 JSON：checks（校验）+ images.a / images.b（PNG）
# 线上出图地址同域，无需 token，也不碰任何设备`}
            />
          </div>
        </div>
      </section>

      <section className="try-block">
        <div className="try-section-head">
          <div>
            <span className="eyebrow">THREE WAYS IN</span>
            <h2>看你手上有什么，选一条</h2>
          </div>
          <p>三条路互不排斥，也可以先做一条跑通，再补另一条。</p>
        </div>
        <div className="try-tracks">
          <article>
            <span>轨 1</span>
            <b>我有网页 / 静态站</b>
            <p>提交 GitHub 仓库，平台按墨水屏规则校验后托管。设备显示海报卡 + 二维码，手机扫码进入完整交互。</p>
            <Link className="ghost" href="/developer">
              去导入仓库 →
            </Link>
          </article>
          <article>
            <span>轨 2</span>
            <b>我有数据 / 接口</b>
            <p>露一个 GET 端点返回上面的 JSON，平台抓取后直接出图。设备显示真实数据，不用扫码。</p>
            <Link className="ghost" href="/developer">
              去配置数据接口 →
            </Link>
          </article>
          <article>
            <span>轨 3</span>
            <b>我自己算好了推过来</b>
            <p>桌面程序、内网服务、跑在本机的脚本：算完调用 content 接口推给平台，让屏跟着你的程序走。</p>
            <Link className="ghost" href="/api-docs">
              看推送接口 →
            </Link>
          </article>
        </div>
      </section>

      <section className="try-block">
        <div className="try-section-head">
          <div>
            <span className="eyebrow">ON REAL DEVICES</span>
            <h2>已经跑在真机上的内容</h2>
          </div>
          <p>每一张都是设备实际会显示的图，不是设计稿。已上架的排前面，后面是平台内置场景。</p>
        </div>
        <div className="try-wall">
          {showcase === null ? <div className="try-loading">读取作品…</div> : null}
          {wallItems.map((item) =>
            item.kind === "app" ? (
              <article key={item.app_id}>
                <div className="try-wall-imgs">
                  <img src={`${BASE}/backend/api/preview?appId=${encodeURIComponent(item.app_id)}&persona=DUAL_A`} alt="A 屏" />
                  <img
                    src={`${BASE}/backend/api/preview?appId=${encodeURIComponent(item.app_id)}&persona=DUAL_B&colors=4`}
                    alt="B 屏"
                  />
                </div>
                <b>{item.name}</b>
                <p>{item.description || "暂无说明"}</p>
              </article>
            ) : (
              <article key={item.label}>
                <div className="try-wall-imgs">
                  <img src={`${BASE}/backend/api/preview?${item.a}`} alt="A 屏" />
                  <img src={`${BASE}/backend/api/preview?${item.b}`} alt="B 屏" />
                </div>
                <b>{item.label}</b>
                <p>平台内置场景</p>
              </article>
            ),
          )}
          {wallItems.length < 3 ? (
            <article className="try-wall-slot">
              <div className="try-wall-ghost">?</div>
              <b>你的作品</b>
              <p>上架后这里会显示你的 A / B 双屏，真实用户能在设备上选到。</p>
              <Link href="/developer">去上架 →</Link>
            </article>
          ) : null}
        </div>
      </section>

      <section className="try-cta">
        <div>
          <h2>想让它真的上屏？</h2>
          <p>
            注册创作者、建一个沙箱应用就能开始推。现在完全免费：审核通过后进入内容库，真实用户能选到你的应用；
            你在自己仓库里的著作权始终归你，平台只承载「设备端呈现」。后续订阅分成会另行公示规则。
          </p>
        </div>
        <div className="try-cta-actions">
          <Link className="try-primary" href="/developer">
            注册创作者 →
          </Link>
          <Link className="ghost" href="/api-docs">
            先读接口规范
          </Link>
        </div>
      </section>
    </div>
  );
}

function Snippet({ id, title, code, copy, copied }) {
  return (
    <div className="try-snippet">
      <div className="try-snippet-head">
        <b>{title}</b>
        <button onClick={() => copy(id, code)}>{copied === id ? "已复制" : "复制"}</button>
      </div>
      <pre>
        <code>{code}</code>
      </pre>
    </div>
  );
}
