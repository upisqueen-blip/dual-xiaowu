"use client";
import { useEffect, useMemo, useState } from "react";

// 后端根：pc-console 把 /developer/backend/* 反向代理到 8104。
const API_BASE = "/developer/backend";

const METHOD_TONE = { get: "m-get", post: "m-post", delete: "m-del" };

function opId(o) {
  return `op-${o.method}-${o.path.replace(/[/{}]/g, "-")}`;
}

function Copy({ text, label = "复制" }) {
  const [ok, setOk] = useState(false);
  async function run() {
    try {
      await navigator.clipboard.writeText(text);
    } catch {
      const ta = document.createElement("textarea");
      ta.value = text;
      document.body.appendChild(ta);
      ta.select();
      document.execCommand("copy");
      ta.remove();
    }
    setOk(true);
    setTimeout(() => setOk(false), 1600);
  }
  return (
    <button type="button" className="api-copy" onClick={run}>
      {ok ? "已复制" : label}
    </button>
  );
}

function Code({ children }) {
  return (
    <pre className="api-code">
      <code>{children}</code>
    </pre>
  );
}

// 轻量行内格式：**粗体** 与 `代码`，避免把 Markdown 符号直接漏给用户。
function Rich({ text }) {
  const parts = String(text || "").split(/(\*\*[^*]+\*\*|`[^`]+`)/g);
  return (
    <>
      {parts.map((p, i) => {
        if (p.startsWith("**") && p.endsWith("**")) return <b key={i}>{p.slice(2, -2)}</b>;
        if (p.startsWith("`") && p.endsWith("`"))
          return (
            <code key={i} className="api-inline">
              {p.slice(1, -1)}
            </code>
          );
        return <span key={i}>{p}</span>;
      })}
    </>
  );
}

// 描述里用 ``` 包住的示例单独渲染成代码块，其余按段落显示。
function Desc({ text }) {
  const chunks = String(text || "").split(/```[a-zA-Z]*\n([\s\S]*?)```/g);
  return (
    <>
      {chunks.map((chunk, i) =>
        i % 2 === 1 ? (
          <div key={i} className="api-inline-code">
            <div className="api-block-head">
              <h4>示例</h4>
              <Copy text={chunk.trim()} />
            </div>
            <Code>{chunk.trim()}</Code>
          </div>
        ) : chunk.trim() ? (
          <p key={i} className="api-op-desc">
            <Rich text={chunk.trim()} />
          </p>
        ) : null
      )}
    </>
  );
}

export default function ApiDocs({ compact = false, initialSpec = null, initialMd = "" }) {
  const [spec, setSpec] = useState(initialSpec);
  const [md, setMd] = useState(initialMd);
  const [err, setErr] = useState("");
  const [active, setActive] = useState("");

  useEffect(() => {
    if (initialSpec) return; // 服务端已直出，避免重复请求
    fetch(`${API_BASE}/openapi/isv.json`, { cache: "no-store" })
      .then(async (r) => {
        if (!r.ok) throw new Error(`HTTP ${r.status}`);
        return r.json();
      })
      .then(setSpec)
      .catch((e) => setErr(`加载接口规范失败：${e.message}`));
    fetch(`${API_BASE}/openapi/isv.md`, { cache: "no-store" })
      .then((r) => (r.ok ? r.text() : ""))
      .then(setMd)
      .catch(() => {});
  }, [initialSpec]);

  const operations = useMemo(() => {
    if (!spec) return [];
    return Object.entries(spec.paths || {}).flatMap(([path, ops]) =>
      Object.entries(ops).map(([method, op]) => ({ path, method, ...op }))
    );
  }, [spec]);

  useEffect(() => {
    if (!operations.length) return;
    const obs = new IntersectionObserver(
      (entries) => {
        const hit = entries.find((e) => e.isIntersecting);
        if (hit) setActive(hit.target.id);
      },
      { rootMargin: "-90px 0px -70% 0px" }
    );
    operations.forEach((o) => {
      const el = document.getElementById(opId(o));
      if (el) obs.observe(el);
    });
    return () => obs.disconnect();
  }, [operations]);

  if (err) return <div className="api-error">{err}</div>;
  if (!spec) return <div className="api-loading">正在加载接口规范…</div>;

  const info = spec.info || {};
  const baseUrl = spec.servers?.[0]?.variables?.baseUrl?.default || spec.servers?.[0]?.url || "";
  const scopes = spec["x-scopes"] || {};
  const flows = spec["x-flow"] || [];
  const schemas = Object.entries(spec.components?.schemas || {});

  return (
    <div className="api-docs">
      {!compact ? (
        <aside className="api-nav">
          <div className="api-nav-box">
            <p className="api-nav-title">ON THIS PAGE</p>
            <a href="#api-overview">概览与鉴权</a>
            <a href="#api-flow">接入四步</a>
            <p className="api-nav-title">接口</p>
            {operations.map((o) => (
              <a key={o.path + o.method} href={`#${opId(o)}`} className={active === opId(o) ? "on" : ""}>
                <em>{o.method}</em>
                {o.summary}
              </a>
            ))}
            <p className="api-nav-title">数据结构</p>
            {schemas.map(([n]) => (
              <a key={n} href={`#schema-${n}`}>
                {n}
              </a>
            ))}
          </div>
        </aside>
      ) : null}

      <div className="api-main">
        <section id="api-overview" className="api-card">
          <div className="api-head">
            <div>
              <p className="api-eyebrow">ISV API REFERENCE</p>
              <h2>{info.title}</h2>
              <p className="api-sub">
                v{info.version} · OpenAPI 3.0.3 · {operations.length} 个接口 · 同一份规范在线看或下载
              </p>
            </div>
            <div className="api-actions">
              <a className="api-btn primary" href={`${API_BASE}/openapi/isv.json?download=1`}>
                下载 OpenAPI (JSON)
              </a>
              <a className="api-btn" href={`${API_BASE}/openapi/isv.yaml?download=1`}>
                YAML
              </a>
              <a className="api-btn" href={`${API_BASE}/openapi/isv.md?download=1`}>
                Markdown
              </a>
              {md ? <Copy text={md} label="复制全文（喂给 AI）" /> : null}
              <a className="api-btn" href={`${API_BASE}/openapi/isv.json`} target="_blank" rel="noreferrer">
                在线打开
              </a>
            </div>
          </div>

          <p className="api-desc">
            <Rich text={info.description} />
          </p>

          <div className="api-facts">
            <div>
              <span>Base URL</span>
              <b className="mono">{baseUrl}</b>
            </div>
            <div>
              <span>鉴权</span>
              <b className="mono">X-App-Key</b>
            </div>
            <div>
              <span>编码</span>
              <b>application/json · UTF-8</b>
            </div>
            <div>
              <span>错误体</span>
              <b className="mono">{'{"error","message"}'}</b>
            </div>
          </div>

          <h3 className="api-h3">权限 scopes</h3>
          <table className="api-table">
            <tbody>
              {Object.entries(scopes).map(([k, v]) => (
                <tr key={k}>
                  <td className="mono key">{k}</td>
                  <td>{v}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </section>

        <section id="api-flow" className="api-card">
          <h3 className="api-h3 first">接入四步</h3>
          <div className="api-flow">
            {flows.map((f) => (
              <div key={f.step}>
                <i>{f.step}</i>
                <b>{f.name}</b>
                <small>{f.detail}</small>
              </div>
            ))}
          </div>
        </section>

        {operations.map((op) => (
          <Operation key={op.path + op.method} op={op} baseUrl={baseUrl} />
        ))}

        <section className="api-card">
          <h3 className="api-h3 first">数据结构</h3>
          <div className="api-schemas">
            {schemas.map(([name, schema]) => (
              <div key={name} id={`schema-${name}`} className="api-schema">
                <b>{name}</b>
                {schema.type === "array" ? (
                  <p className="api-muted">数组，元素见 DeviceEvent。</p>
                ) : (
                  <table className="api-table tight">
                    <thead>
                      <tr>
                        <th>字段</th>
                        <th>类型</th>
                        <th>必填</th>
                      </tr>
                    </thead>
                    <tbody>
                      {Object.entries(schema.properties || {}).map(([field, prop]) => {
                        const t =
                          prop.type === "array" ? `array<${prop.items?.type || "object"}>` : prop.type || "object";
                        const required = (schema.required || []).includes(field);
                        return (
                          <tr key={field}>
                            <td className="mono key">{field}</td>
                            <td className="api-muted">{t}</td>
                            <td>{required ? <em className="req">是</em> : <span className="api-muted">否</span>}</td>
                          </tr>
                        );
                      })}
                    </tbody>
                  </table>
                )}
              </div>
            ))}
          </div>
        </section>

        <p className="api-foot">
          本页数据来自后端 <code>/openapi/isv.json</code>，与线上路由同一份真源；接口变更后刷新即更新。
        </p>
      </div>
    </div>
  );
}

function Operation({ op, baseUrl }) {
  const params = op.parameters || [];
  const reqBody = op.requestBody?.content?.["application/json"];
  const responses = Object.entries(op.responses || {});
  return (
    <section id={opId(op)} className="api-card api-op">
      <div className="api-op-head">
        <span className={`api-method ${METHOD_TONE[op.method] || ""}`}>{op.method}</span>
        <code className="api-path">{op.path}</code>
        <span className="api-tag">{op.tags?.[0] || ""}</span>
        <Copy text={`${op.method.toUpperCase()} ${baseUrl}${op.path}`} label="复制地址" />
      </div>
      <h3 className="api-op-title">{op.summary}</h3>
      <Desc text={op.description} />

      {params.length ? (
        <table className="api-table tight">
          <thead>
            <tr>
              <th>参数</th>
              <th>位置</th>
              <th>必填</th>
              <th>说明</th>
            </tr>
          </thead>
          <tbody>
            {params.map((p) => (
              <tr key={p.name}>
                <td className="mono key">{p.name}</td>
                <td className="api-muted">{p.in}</td>
                <td>{p.required ? <em className="req">是</em> : <span className="api-muted">否</span>}</td>
                <td>{p.description}</td>
              </tr>
            ))}
          </tbody>
        </table>
      ) : null}

      {reqBody ? (
        <div className="api-block">
          <div className="api-block-head">
            <h4>请求体示例</h4>
            <Copy text={JSON.stringify(reqBody.example ?? {}, null, 2)} />
          </div>
          <Code>{JSON.stringify(reqBody.example ?? {}, null, 2)}</Code>
        </div>
      ) : null}

      <div className="api-block">
        <h4>响应</h4>
        {responses.map(([code, resp]) => {
          const example = resp.content?.["application/json"]?.example;
          return (
            <div key={code} className="api-resp">
              <div className="api-resp-head">
                <span className={`api-status ${code.startsWith("2") ? "ok" : code.startsWith("4") ? "bad" : ""}`}>
                  {code}
                </span>
                <span>{resp.description}</span>
              </div>
              {example !== undefined ? <Code>{JSON.stringify(example, null, 2)}</Code> : null}
            </div>
          );
        })}
      </div>
    </section>
  );
}
