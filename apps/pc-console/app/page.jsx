"use client";
import Link from "next/link";
import ApiDocs from "./api-docs/ApiDocs";
import { useEffect, useState } from "react";

// 创作者门户挂在根路径 /,API 走同域 /backend(由 next.config 反代到后端 8104)
const BASE = "";
const scenes=[["office","办公","会议、日程、专注"],["study","学习","课程、作业、单词"],["family","家庭","留言、相册、纪念日"],["finance","炒股","行情、自选与预警"],["wellness","养生","用药、运动、健康提醒"],["custom","自定义","创造你的双屏体验"]];
const statusText={sandbox:"沙箱联调",in_review:"审核中",published:"已上线",rejected:"已驳回"};
const roleText={developer:"开发者",content_creator:"内容创作者",ip_owner:"IP 商",channel_partner:"渠道商",other:"其他"};
async function api(path,{token,method="GET",body}={}){const res=await fetch(`${BASE}/backend${path}`,{method,headers:{...(token?{Authorization:`Bearer ${token}`}:{}) ,...(body?{"Content-Type":"application/json"}:{})},body:body?JSON.stringify(body):undefined,cache:"no-store"});const data=await res.json().catch(()=>({}));if(!res.ok)throw new Error(typeof data.detail==="string"?data.detail:`请求失败 HTTP ${res.status}`);return data}

export default function DeveloperCenter(){
 const [token,setToken]=useState(""),[me,setMe]=useState(null),[apps,setApps]=useState([]),[view,setView]=useState("home"),[error,setError]=useState(""),[notice,setNotice]=useState(""),[hardware,setHardware]=useState(null),[selected,setSelected]=useState(null),[importing,setImporting]=useState(false),[importResult,setImportResult]=useState(null),[onbStep,setOnbStep]=useState(0);
 useEffect(()=>{setToken(localStorage.getItem("xiaowu-developer-token")||"");api("/developer/v1/hardware").then(setHardware)},[]);
 useEffect(()=>{if(!token)return;Promise.all([api("/developer/v1/me",{token}),api("/developer/v1/apps",{token})]).then(([u,a])=>{setMe(u);setApps(a)}).catch(()=>{localStorage.removeItem("xiaowu-developer-token");setToken("")})},[token]);
 function done(message){setNotice(message);setError("");setTimeout(()=>setNotice(""),2400)}
 async function auth(e,kind){e.preventDefault();setError("");const f=new FormData(e.currentTarget),body=Object.fromEntries(f.entries());if(kind==="register"){body.accepted_terms=!!f.get("accepted_terms");body.accepted_content_policy=!!f.get("accepted_content_policy");body.agreement_version="2026-10-23"}try{const r=await api(`/developer/v1/auth/${kind}`,{method:"POST",body});localStorage.setItem("xiaowu-developer-token",r.token);setToken(r.token)}catch(err){setError(err.message)}}
 async function createApp(e){e.preventDefault();const f=new FormData(e.currentTarget);try{const r=await api("/developer/v1/apps",{token,method:"POST",body:Object.fromEntries(f.entries())});setApps([r,...apps]);setSelected(r);done("沙箱已开通，请立即保存本次显示的 Token")}catch(err){setError(err.message)}}
 async function saveModel(e){e.preventDefault();const f=new FormData(e.currentTarget);try{await api(`/developer/v1/apps/${selected.app_id}/model`,{token,method:"PUT",body:Object.fromEntries(f.entries())});done("模型配置已加密保存，密钥不会再次回显")}catch(err){setError(err.message)}}
 async function submit(app){try{await api(`/developer/v1/apps/${app.app_id}/submit`,{token,method:"POST",body:{version:app.version,notes:"开发者中心提交审核"}});setApps(apps.map(x=>x.app_id===app.app_id?{...x,status:"in_review"}:x));done("已提交审核，只有平台管理员可发布上线")}catch(err){setError(err.message)}}
 async function importGithub(e,appId){e.preventDefault();if(!appId){setError("请先创建或选择一个应用");return}const f=new FormData(e.currentTarget);setImporting(true);setImportResult(null);setError("");try{const r=await api(`/developer/v1/apps/${appId}/import-github`,{token,method:"POST",body:{repo_url:f.get("repo_url"),branch:f.get("branch")||"",entry:f.get("entry")||""}});setImportResult(r);setApps(apps.map(x=>x.app_id===appId?{...x,github:{repo_url:r.repo_url,commit_sha:r.commit_sha,entry:r.entry,verdict:r.report.verdict,imported_at:new Date().toISOString(),preview_url:r.preview_url,report:r.report}}:x));done(r.report.passed?`已部署到沙箱（${r.commit_sha||"HEAD"}），可打开预览`:"已导入沙箱，但仍有规则未通过，请按报告修正后重新导入")}catch(err){setError(err.message)}finally{setImporting(false)}}
 function logout(){localStorage.removeItem("xiaowu-developer-token");setToken("");setMe(null)}
 function handleDevGuideStep(i){if(i===0)setView("apps");else if(i===1)window.open("/","_blank");else if(i===2)window.open("/admin","_blank")}
 if(!token)return <div className="portal-shell"><PortalHeader/><div className="portal-intro"><div><span className="eyebrow">XIAOWU CREATOR PLATFORM</span><h1>让每一块墨水屏<br/>都有新的可能</h1><p>这里不只欢迎开发者。内容创作者、IP 商、渠道商以及每一位有好创意的人，都可以为小悟双屏贡献内容。</p><div className="trust-row"><b>人人可创作</b><b>双屏硬件规范</b><b>隔离沙箱</b><b>管理员发布</b></div></div><AuthPanel onAuth={auth} error={error}/></div><HardwareStrip hardware={hardware}/></div>;
 return <div className="creator-app"><aside className="creator-nav"><div className="creator-logo"><img src={`${BASE}/logo.png`} alt=""/><div><b>小悟众创</b><span>CREATOR HUB</span></div></div><div className="creator-user"><i>{me?.name?.slice(0,1)}</i><div><b>{me?.name}</b><span>{roleText[me?.role]||"创作者"}{me?.organization?` · ${me.organization}`:""}</span></div></div><nav>{[["home","工作台"],["hardware","硬件规范"],["apps","我的内容"],["onboarding","接入引导"],["api","API 文档"],["ai","AI 能力"],["guide","配置引导"],["policy","协议与安全"]].map(x=><button key={x[0]} className={view===x[0]?"on":""} onClick={()=>{if(x[0]==="onboarding")setOnbStep(0);setView(x[0])}}>{x[1]}</button>)}</nav><button className="logout" onClick={logout}>退出登录</button></aside><main className="creator-main"><header className="creator-head"><div><span className="eyebrow">CREATOR CENTER</span><h1>{({home:"众创工作台",hardware:"双屏硬件规范",apps:"应用与内容",onboarding:"接入引导",api:"ISV API 文档",ai:"大模型配置",guide:"配置引导",policy:"协议与内容安全"})[view]}</h1></div><div className="env-pill"><i/> SANDBOX 环境</div></header>{error&&<div className="toast error">{error}</div>}{notice&&<div className="toast success">{notice}</div>}{view==="home"&&<Dashboard apps={apps} setView={setView} onOpenOnboard={(i)=>{setOnbStep(i);setView("onboarding")}}/>} {view==="hardware"&&<Hardware hardware={hardware}/>} {view==="apps"&&<Apps apps={apps} onCreate={createApp} onSubmit={submit} selected={selected} setSelected={setSelected} onImport={importGithub} importing={importing} importResult={importResult} token={token} onRefresh={()=>api("/developer/v1/apps",{token}).then(setApps).catch(()=>{})}/>} {view==="ai"&&<ModelConfig apps={apps} selected={selected} setSelected={setSelected} onSave={saveModel}/>} {view==="onboarding"&&<Onboarding apps={apps} token={token} setView={setView} selected={selected} setSelected={setSelected} initialStep={onbStep}/>} {view==="api"&&<ApiDocs/>} {view==="guide"&&<ConfigGuide onStep={handleDevGuideStep}/>} {view==="policy"&&<Policy/>}</main></div>
}
function PortalHeader(){return <header className="portal-head"><div className="creator-logo"><img src={`${BASE}/logo.png`} alt=""/><div><b>小悟众创</b><span>OPEN CREATOR PLATFORM</span></div></div><Link href="/center">平台运营后台</Link></header>}
function AuthPanel({onAuth,error}){const[mode,setMode]=useState("register");return <section className="auth-panel"><div className="auth-tabs"><button className={mode==="register"?"on":""} onClick={()=>setMode("register")}>注册创作者</button><button className={mode==="login"?"on":""} onClick={()=>setMode("login")}>已有账号登录</button></div><h2>{mode==="register"?"加入小悟众创":"欢迎回来"}</h2><p>{mode==="register"?"不要求会写代码，每一种创意身份都可以贡献内容。":"继续管理你的创意、内容与联调应用。"}</p><form onSubmit={e=>onAuth(e,mode)}>{mode==="register"&&<><label>你的身份<select name="role" defaultValue="developer" required><option value="developer">开发者</option><option value="content_creator">内容创作者</option><option value="ip_owner">IP 商</option><option value="channel_partner">渠道商</option><option value="other">其他</option></select></label><label>姓名<input name="name" required placeholder="你的姓名"/></label><label>团队 / 公司<input name="organization" placeholder="选填"/></label></>}<label>邮箱<input name="email" type="email" required placeholder="name@example.com"/></label><label>密码<input name="password" type="password" minLength="8" required placeholder="至少 8 位"/></label>{mode==="register"&&<div className="agreements"><label><input type="checkbox" name="accepted_terms" required/> 我已阅读并同意《开发者服务协议》</label><label><input type="checkbox" name="accepted_content_policy" required/> 我承诺不制作政治有害、色情低俗、暴力违法、侵权或欺诈内容</label></div>}{error&&<div className="form-error">{error}</div>}<button className="primary" type="submit">{mode==="register"?"同意协议并加入众创":"登录众创中心"}</button></form></section>}
function HardwareStrip({hardware}){return <div className="hardware-strip"><span>HARDWARE / S3 DUAL E-INK</span>{hardware?.screens?.map(s=><div key={s.port}><b>{s.port}</b><p>{s.type}</p><small>{s.usage}</small></div>)}</div>}
const QUICK_STEPS=[
  {t:"签发凭证",d:"拿到 App Token，应用的第一把钥匙",api:"POST /credential（后台签发）"},
  {t:"确权绑定",d:"用户点「允许」，建立应用·账号·设备三元授权",api:"pairing/start → 用户确认"},
  {t:"交付数据",d:"静态页或数据接口，二选一",api:"静态页 / 你的 HTTP 端点"},
  {t:"推送上线",d:"按契约推卡片，审核后进生产",api:"POST …/content"},
];
const QUICK_APIS=[
  ["POST","/openapi/v1/pairing/start","申请 6 位绑定码（10 分钟有效）","必需"],
  ["GET","/openapi/v1/pairing/{code}","轮询审批结果，拿到 device_id","必需"],
  ["GET","/openapi/v1/devices/{device_id}","查屏幕规格：尺寸、几色、能力位","建议"],
  ["POST","/openapi/v1/devices/{device_id}/content","推送 e-ink 卡片到 A / B / 双屏","必需"],
  ["GET","/openapi/v1/devices/{device_id}/events","回收「已显示 / 失败」回执","建议"],
];
function QuickStart({apps,onOpen,setView}){
  const summary=`${QUICK_STEPS.length} 步 · ${QUICK_APIS.length} 个接口`;
  return <section className="section-card qs-card">
    <div className="qs-head">
      <div>
        <span className="eyebrow">ISV QUICKSTART</span>
        <h2>接入速览 · {summary}</h2>
        <p className="qs-lead">平台只做三件事：<b>确认这是谁的数据</b> → <b>接收你算好的结果</b> → <b>渲染到双屏</b>。你的数据源、账号体系与算法留在自己那边，平台不连你的数据库，也不索取用户的大模型账号密码。</p>
      </div>
      <div className="qs-head-actions">
        <button className="ghost" onClick={()=>setView("api")}>API 文档 →</button>
        <a className="ghost" href="/backend/openapi/isv.json?download=1">下载 OpenAPI</a>
        <button className="ghost" onClick={()=>onOpen(0)}>打开完整引导 →</button>
      </div>
    </div>
    <div className="qs-steps">
      {QUICK_STEPS.map((s,i)=><button key={s.t} className="qs-step" onClick={()=>onOpen(i)}>
        <i>{i+1}</i>
        <span><b>{s.t}</b><small>{s.d}</small><em>{s.api}</em></span>
      </button>)}
    </div>
    <div className="qs-grid">
      <div>
        <div className="qs-sub-head">
          <h3>要准备的接口</h3>
          <span>规范下载：
            <a href="/backend/openapi/isv.json?download=1">JSON</a> ·
            <a href="/backend/openapi/isv.yaml?download=1">YAML</a> ·
            <a href="/backend/openapi/isv.md?download=1">Markdown</a>
          </span>
        </div>
        <table className="qs-table">
          <thead><tr><th>方法</th><th>接口</th><th>用途</th><th>必需</th></tr></thead>
          <tbody>{QUICK_APIS.map(a=><tr key={a[0]+a[1]}>
            <td><span className={`qs-method m-${a[0].toLowerCase()}`}>{a[0]}</span></td>
            <td><code>{a[1]}</code></td>
            <td>{a[2]}</td>
            <td><span className={`qs-req ${a[3]==="必需"?"on":""}`}>{a[3]}</span></td>
          </tr>)}</tbody>
        </table>
      </div>
      <div>
        <h3>接口协议要点</h3>
        <ul className="qs-proto">
          <li><b>鉴权</b><span>请求头 <code>X-App-Key: &lt;App Token&gt;</code>；Token 只在签发时显示一次，存进钥匙串，别写进日志。</span></li>
          <li><b>先授权再推送</b><span>未授权的设备返回 <code>403 pairing_required</code>，不会静默失败。</span></li>
          <li><b>推送体</b><span><code>{`{template, screen:"A"|"B"|"both", data:{}, expires_in_seconds}`}</code></span></li>
          <li><b>屏幕约束</b><span>400×300，A 屏黑白、B 屏四色；字号 ≥18px，不做动画，单屏信息别超过 6 条。</span></li>
          <li><b>回执语义</b><span><code>202</code> 只代表已入队；真实「已显示 / 失败」走 events 轮询。</span></li>
          <li><b>重试</b><span>失败指数退避；同一内容勿高频重推（耗电、伤屏）。</span></li>
        </ul>
        <p className="qs-note">当前推送须命中已注册模板（notice-card、daily-schedule 等）。像额度环这类自定义卡片，需要先在平台开通 <code>eink-card-v1</code> 免校验模板。</p>
      </div>
    </div>
    {!apps.length?<p className="qs-note">还没有应用。接入第一步是创建一个沙箱应用，凭证、确权、双屏都围绕它进行。</p>:null}
  </section>;
}
function Dashboard({apps,setView,onOpenOnboard}){return <><div className="hero-card"><div><span>从一个好点子开始</span><h2>为双屏创造更安静、更有用的内容</h2><p>FPC1 承载清晰高频信息，FPC2 用有限色彩表达重点。所有创意先在沙箱验证，再由平台审核上线。</p><button onClick={()=>setView("apps")}>创建第一个应用 →</button></div><div className="screen-pair"><div><small>FPC1 · BW</small><b>09:41</b><p>今日专注<br/>深度工作 45 分钟</p></div><div><small>FPC2 · COLOR</small><strong>IDEA<br/>TO<br/>INK</strong></div></div></div><QuickStart apps={apps} onOpen={onOpenOnboard} setView={setView}/><div className="stat-grid"><article><span>我的应用</span><b>{apps.length}</b><small>沙箱与已上线</small></article><article><span>审核中</span><b>{apps.filter(x=>x.status==="in_review").length}</b><small>等待平台处理</small></article><article><span>已上线</span><b>{apps.filter(x=>x.status==="published").length}</b><small>生产环境内容</small></article></div><section className="section-card"><h2>推荐创作场景</h2><div className="scene-grid">{scenes.map((s,i)=><article key={s[0]}><i>0{i+1}</i><b>{s[1]}</b><p>{s[2]}</p></article>)}</div></section></>}
function Hardware({hardware}){return <><div className="spec-grid">{hardware?.screens?.map((s,i)=><article key={s.port} className={`spec-card tone-${i}`}><span>{s.port} / SCREEN {s.id}</span><h2>{s.type}</h2><p>{s.usage}</p><div>{s.colors.map(c=><i key={c} title={c} className={`ink-${c}`}/>)}</div></article>)}</div><section className="section-card"><h2>电子墨水屏适配红线</h2><div className="rule-list">{hardware?.rules?.map((r,i)=><div key={r}><b>{String(i+1).padStart(2,"0")}</b><p>{r}</p></div>)}</div><p className="subtle">刷新策略：{hardware?.refresh_strategy}</p></section></>}
const ONB_STEPS=[
  {t:"签发凭证",d:"App Token 是应用调用平台接口的身份"},
  {t:"确权绑定",d:"用户显性授权：应用 · 账号 · 设备"},
  {t:"交付数据",d:"静态页或数据接口，二选一"},
  {t:"双屏与提交",d:"A 屏此刻、B 屏趋势，审核后上线"},
];
const PAIR_BASE="/admin/pair";

function CopyField({text}){
  const [ok,setOk]=useState(false);
  async function copy(){
    try{await navigator.clipboard.writeText(text);setOk(true);setTimeout(()=>setOk(false),1600)}
    catch{setOk(false)}
  }
  return <div className="onb-copy"><code>{text}</code><button onClick={copy}>{ok?"已复制":"复制"}</button></div>
}

function Onboarding({apps,token,setView,selected,setSelected,initialStep=0}){
  const [appId,setAppId]=useState(selected?.app_id||apps[0]?.app_id||"");
  const [step,setStep]=useState(initialStep||0);
  const [cred,setCred]=useState(null);
  const [newToken,setNewToken]=useState("");
  const [pair,setPair]=useState(null);
  const [pairStatus,setPairStatus]=useState(null);
  const [busy,setBusy]=useState(false);
  const [err,setErr]=useState("");

  useEffect(()=>{if(!appId&&apps[0])setAppId(apps[0].app_id)},[apps,appId]);
  useEffect(()=>{
    if(!appId||!token)return;
    api(`/developer/v1/apps/${appId}/credential`,{token}).then(setCred).catch(()=>setCred(null));
    setPair(null);setPairStatus(null);setNewToken("");
  },[appId,token]);
  // 绑定码状态轮询：拿到 approved/denied/expired 后自动停止，避免无意义请求。
  useEffect(()=>{
    if(!pair||!appId||!token)return;
    let stop=false;
    const tick=async()=>{
      try{
        const s=await api(`/developer/v1/apps/${appId}/pairing/${pair.code}`,{token});
        if(stop)return;
        setPairStatus(s);
        if(["approved","denied","expired"].includes(s.status))stop=true;
      }catch{/* 网络抖动忽略，下次再查 */}
    };
    const timer=setInterval(()=>{if(stop){clearInterval(timer);return}tick()},3000);
    return ()=>{stop=true;clearInterval(timer)};
  },[pair,appId,token]);

  const current=apps.find(a=>a.app_id===appId);
  const appName=current?.name||"";

  async function issueCredential(){
    if(!appId)return;
    if(cred?.issued&&!window.confirm("重新签发会使旧 App Token 立即失效，正在联调的应用需同步更新。继续？"))return;
    setBusy(true);setErr("");
    try{
      const r=await api(`/developer/v1/apps/${appId}/credential`,{token,method:"POST"});
      setNewToken(r.app_token||"");
      setCred({issued:true,scopes:r.scopes||[],created_at:r.issued_at});
    }catch(e){setErr(e.message)}finally{setBusy(false)}
  }

  async function startPairing(){
    if(!appId)return;
    setBusy(true);setErr("");setPairStatus(null);
    try{
      const r=await api(`/developer/v1/apps/${appId}/pairing/start`,{token,method:"POST",
        body:{label:appName,scopes:["device.content.write"]}});
      setPair(r);
    }catch(e){setErr(e.message)}finally{setBusy(false)}
  }

  if(!apps.length)return <section className="section-card">
    <h2>接入引导</h2>
    <p className="onb-lead">还没有应用。接入的第一步是创建一个沙箱应用，后续的凭证、确权、双屏都围绕它进行。</p>
    <button className="ghost" onClick={()=>setView("apps")}>去创建应用</button>
  </section>;

  const pairUrl=pair?`${PAIR_BASE}?code=${pair.code}`:"";

  return <>
    <section className="section-card onb-intro">
      <span className="eyebrow">ISV ONBOARDING</span>
      <h2>第三方接入引导</h2>
      <p className="onb-lead">
        平台只做三件事：<b>确认这是谁的数据</b> → <b>接收你算好的结果</b> → <b>渲染到双屏</b>。
        你的数据源、账号体系与算法都留在你自己那边，平台不会连接你的数据库，也不会索取用户的大模型账号密码。
      </p>
      <div className="onb-layers">
        <div><i>1</i><b>确权</b><small>应用 · 账号 · 设备，由用户本人点「允许」</small></div>
        <div><i>2</i><b>业务</b><small>你的应用取数、计算，按契约交付结果</small></div>
        <div><i>3</i><b>渲染</b><small>平台出 e-ink 卡片，A/B 双屏各司其职</small></div>
      </div>
    </section>

    <section className="section-card">
      <div className="onb-picker">
        <label>正在配置的应用
          <select value={appId} onChange={e=>setAppId(e.target.value)}>
            {apps.map(a=><option key={a.app_id} value={a.app_id}>{a.name}</option>)}
          </select>
        </label>
        <span className="onb-state">{statusText[current?.status]||"沙箱"}</span>
        <button className="ghost" onClick={()=>{setSelected(current);setView("apps")}}>管理我的应用</button>
      </div>

      <div className="onb-steps">
        {ONB_STEPS.map((s,i)=><button key={s.t} className={`onb-step ${step===i?"on":""}`} onClick={()=>setStep(i)}>
          <i>{i+1}</i><span><b>{s.t}</b><small>{s.d}</small></span>
        </button>)}
      </div>

      {err&&<p className="form-error" style={{marginTop:12}}>{err}</p>}

      {step===0&&<div className="onb-body">
        <h3>第 1 步 · 签发接入凭证</h3>
        <p>App Token 是应用调用平台接口的唯一身份。它与内容应用共用同一个 <code>app_id</code>，确权、推送、渲染都指向这一个应用，不需要维护两套。</p>
        <div className="onb-stat">
          <div><span>应用</span><b>{appName||"—"}</b></div>
          <div><span>凭证状态</span><b className={cred?.issued?"ok":"warn"}>{cred?.issued?"已签发":"未签发"}</b></div>
          <div><span>权限</span><b>{(cred?.scopes?.length?cred.scopes:["device.content.write"]).join(" / ")}</b></div>
        </div>
        {newToken?<div className="onb-token">
          <b>请立即保存，此 Token 只显示一次</b>
          <CopyField text={newToken}/>
          <small>调用平台接口时放进请求头：<code>X-App-Key: {newToken.slice(0,14)}…</code></small>
        </div>:null}
        <div className="onb-actions">
          <button className="ghost" disabled={busy||!appId} onClick={issueCredential}>{cred?.issued?"重新签发 Token":"签发 App Token"}</button>
          {cred?.issued?<button className="ghost" onClick={()=>setStep(1)}>下一步：确权绑定 →</button>:null}
        </div>
        {cred?.issued?<p className="onb-note">Token 已加密存储，后台不再回显；重新签发会使旧 Token 立即失效。</p>:null}
      </div>}

      {step===1&&<div className="onb-body">
        <h3>第 2 步 · 确权绑定</h3>
        <p>平台必须先知道「这块屏该显示谁的数据」。流程是：应用申请一次性绑定码 → 用户在自己的小悟后台输入并点「允许」→ 平台建立一条 应用 · 账号 · 设备 的三元授权。授权后你才能拿到 <code>device_id</code>，也才能推送。</p>
        <div className="onb-flow">
          <div><b>应用申请</b><small>POST /openapi/v1/pairing/start</small></div>
          <i>→</i>
          <div><b>用户确认</b><small>输入 6 位绑定码，显性点「允许」</small></div>
          <i>→</i>
          <div><b>拿到 device_id</b><small>建立授权，随后可推送</small></div>
        </div>
        <div className="onb-actions">
          <button className="ghost" disabled={busy||!appId} onClick={startPairing}>{pair?"重新生成绑定码":"生成绑定码"}</button>
          {pair?<a className="button ghost" href={pairUrl} target="_blank" rel="noreferrer">打开授权页 →</a>:null}
        </div>
        {pair?<div className="onb-pair">
          <div className="onb-code-box">
            <span className="onb-code">{pair.display_code}</span>
            <small>{Math.max(1,Math.round((pair.expires_in||600)/60))} 分钟内有效 · 请让用户在「授权管理」输入</small>
          </div>
          <CopyField text={pairUrl}/>
          <div className="onb-poll">
            {pairStatus?.status==="approved"
              ? <div className="onb-ok"><b>已授权</b><small>device_id：<code>{pairStatus.device_id}</code>，之后用它推送</small></div>
              : pairStatus?.status==="denied"
                ? <div className="onb-bad">用户拒绝了本次申请</div>
                : pairStatus?.status==="expired"
                  ? <div className="onb-bad">绑定码已过期，请重新生成</div>
                  : <div className="onb-wait">等待用户确认…（每 3 秒自动查询）</div>}
          </div>
        </div>:null}
        {pairStatus?.status==="approved"?<div className="onb-snippet">
          <b>拿到 device_id 后这样推送</b>
          <pre>{`POST /openapi/v1/devices/${pairStatus.device_id}/content
X-App-Key: <你的 App Token>
{"template":"dev-weather","screen":"A","data":{…}}`}</pre>
          <small>未授权的设备会被平台直接拒绝（403 pairing_required），这也是对其他用户设备的保护。</small>
        </div>:null}
      </div>}

      {step===2&&<div className="onb-body">
        <h3>第 3 步 · 交付数据</h3>
        <p>两条轨二选一，判据很简单：<b>应用有没有可被浏览器打开的网页</b>。</p>
        <div className="onb-tracks">
          <article>
            <span>轨 1 · 双屏适配 UI</span>
            <b>有网页版</b>
            <p>提交 GitHub 静态工程，平台按墨水屏规则校验后托管。设备显示海报卡 + 二维码，手机扫码进入完整交互页面。</p>
            <button className="ghost" onClick={()=>{setSelected(current);setView("apps")}}>去导入静态工程</button>
          </article>
          <article>
            <span>轨 2 · 数据接口</span>
            <b>无网页，但有数据</b>
            <p>暴露一个 GET 端点，返回契约 JSON（标题 + 1~6 个键值）。平台实时抓取并渲染成卡片，设备直接显示真实数据，不用扫码。</p>
            <button className="ghost" onClick={()=>{setSelected(current);setView("apps")}}>去配置数据接口</button>
          </article>
        </div>
        <div className="onb-warn">
          桌面应用、跑在用户内网的服务（NAT 后面）无法被平台主动拉取，请改用<b>推送</b>：你在本机算好后调用上面的 content 接口推给平台。
        </div>
        <div className="onb-actions">
          <a className="button ghost" href="/api-docs" target="_blank" rel="noreferrer">打开 API 文档（可下载规范）→</a>
          <a className="button ghost" href="/backend/openapi/isv.json?download=1">下载 OpenAPI JSON</a>
        </div>
        <div className="onb-actions">
          <button className="ghost" onClick={()=>setStep(3)}>下一步：双屏与提交 →</button>
        </div>
      </div>}

      {step===3&&<div className="onb-body">
        <h3>第 4 步 · 双屏设计与提交</h3>
        <p>两块屏不是「一分为二」，而是回答两个不同问题。最容易被审核打回的原因是<b>两屏内容重复</b>。</p>
        <div className="onb-screens">
          <article><b>A 屏</b><small>回答「我现在还有多少」</small><p>主指标、百分比、是否告警。黑白刷新快，适合高频变化。</p></article>
          <article><b>B 屏</b><small>回答「接下来会怎样」</small><p>倒计时、趋势、预计耗尽时间。四色，可用红/黄强调。</p></article>
        </div>
        <div className="onb-checks">
          {["两屏内容不重复，各自回答不同问题","字号 ≥ 18px，隔一臂距离可读","只用黑 / 白 / 红 / 黄四色，不做渐变","数据超过 6 项时只保留最关键的","已用「设备出图预览」确认过实际效果","敏感数据不写日志，App Token 存系统钥匙串"].map(t=><div key={t}><i>✓</i>{t}</div>)}
        </div>
        <div className="onb-actions">
          <button className="ghost" onClick={()=>{setSelected(current);setView("apps")}}>去预览并提交审核</button>
          <button className="ghost" onClick={()=>setStep(0)}>回到第 1 步</button>
        </div>
        <p className="onb-note">提交后由平台核对设备出图再上线；上线后你仍可推送更新内容，无需重新审核版式。</p>
      </div>}
    </section>
  </>;
}

function Apps({apps,onCreate,onSubmit,selected,setSelected,onImport,importing,importResult,token,onRefresh}){
  const [preview,setPreview]=useState(null);
  return <>
    <section className="section-card">
      <h2>创建沙箱应用</h2>
      <form className="inline-form" onSubmit={onCreate}>
        <input name="name" required placeholder="应用名称"/>
        <select name="scene">{scenes.map(s=><option key={s[0]} value={s[0]}>{s[1]}</option>)}</select>
        <input name="description" placeholder="一句话说明创意"/>
        <button>开通联调</button>
      </form>
    </section>
    <GithubImport apps={apps} selected={selected} setSelected={setSelected} onImport={onImport} importing={importing} result={importResult}/>
    <DataSourceForm apps={apps} selected={selected} setSelected={setSelected} token={token} onDone={onRefresh}/>
    <section className="section-card">
      <div className="section-title"><h2>我的应用</h2><span>{apps.length} APPS</span></div>
      {!apps.length
        ? <div className="empty-state">还没有应用。创建后会获得仅显示一次的沙箱 Token。</div>
        : <div className="app-list">{apps.map(a=>
            <article key={a.app_id} onClick={()=>setSelected(a)} className={selected?.app_id===a.app_id?"selected":""}>
              <div>
                <span className={`status ${a.status}`}>{statusText[a.status]}</span>
                <h3>{a.name}</h3>
                <code>{a.app_id}</code>
                <p>{a.description||"暂无描述"}</p>
                {a.track&&<div className="app-track">提交轨：{a.track==="data"?"数据接口":a.track==="static"?"双屏适配UI":"未配置"}{a.track==="data"&&a.data_source?` · ${a.data_source.verdict==="pass"?"已通过":a.data_source.verdict==="warn"?"有建议":"未通过"}`:""}</div>}
                {a.github&&<div className={`gh-chip ${a.github.verdict}`}>
                  <i>{verdictText(a.github.verdict)}</i>
                  <small>{a.github.repo_url.replace("https://github.com/","")} · {a.github.commit_sha||"HEAD"}</small>
                  <a href={a.github.preview_url} target="_blank" rel="noreferrer" onClick={e=>e.stopPropagation()}>沙箱预览 →</a>
                </div>}
                {a.sandbox_token&&<div className="token-once"><b>仅显示一次</b><code>{a.sandbox_token}</code></div>}
              </div>
              <div>
                <small>{scenes.find(s=>s[0]===a.scene)?.[1]}</small>
                <button className="ghost" onClick={e=>{e.stopPropagation();setPreview(a.app_id)}}>设备出图预览</button>
                {["sandbox","rejected"].includes(a.status)&&<button onClick={e=>{e.stopPropagation();onSubmit(a)}}>提交审核</button>}
              </div>
            </article>
          )}</div>}
    </section>
    {preview&&<DevicePreviewModal appId={preview} onClose={()=>setPreview(null)}/>}
  </>;
}

function verdictText(v){return v==="fail"?"规则未通过":v==="warn"?"已部署 · 有建议":"已部署"}

function GithubImport({apps,selected,setSelected,onImport,importing,result}){
  const [appId,setAppId]=useState("");
  const current=appId||selected?.app_id||apps[0]?.app_id||"";
  return <section className="section-card">
    <div className="section-title"><h2>从 GitHub 导入工程</h2><span className="gh-tag">SANDBOX</span></div>
    <p className="subtle">支持公开仓库的纯静态工程（HTML / CSS / JS）。导入后自动按 4.2 寸双屏墨水屏规则校验：版面 400×300、字号 ≥18px、无动画与视频、色板限定黑/白/红/黄。存在不通过项时无法提交审核。</p>
    <form className="gh-form" onSubmit={e=>onImport(e,current)}>
      <label>目标应用<select value={current} onChange={e=>{setAppId(e.target.value);setSelected(apps.find(a=>a.app_id===e.target.value)||null)}} required>
        <option value="">{apps.length?"请选择应用":"请先在上方创建应用"}</option>
        {apps.map(a=><option key={a.app_id} value={a.app_id}>{a.name}</option>)}
      </select></label>
      <label>GitHub 仓库<input name="repo_url" required placeholder="https://github.com/owner/repo"/></label>
      <label>分支<input name="branch" placeholder="留空 = 默认分支"/></label>
      <label>入口文件<input name="entry" placeholder="留空 = 自动查找 index.html"/></label>
      <button className="primary" disabled={importing||!current}>{importing?"导入并校验中…":"导入并校验"}</button>
    </form>
    {result&&<ImportReport result={result}/>}
  </section>;
}

function ImportReport({result}){
  const report=result.report||{},s=report.summary||{};
  return <div className="gh-report">
    <div className={`gh-verdict ${report.verdict}`}>
      <b>{report.verdict==="pass"?"适配规则全部通过":report.verdict==="warn"?"已通过，有优化建议":"有规则未通过"}</b>
      <span>{s.pass||0} 项通过 · {s.warn||0} 项建议 · {s.fail||0} 项不通过</span>
      <a href={result.preview_url} target="_blank" rel="noreferrer">打开沙箱预览 →</a>
    </div>
    <small className="gh-meta">{result.repo_slug} · {result.commit_sha||"HEAD"} · {result.file_count} 个文件 · 入口 {result.entry}</small>
    <div className="check-list">{(report.checks||[]).map(c=>
      <div key={c.id} className={`check ${c.status}`}>
        <i>{c.status==="pass"?"✓":c.status==="warn"?"!":"×"}</i>
        <div><b>{c.label}</b><p>{c.detail}</p></div>
      </div>
    )}</div>
  </div>;
}
function DataSourceForm({apps,selected,setSelected,token,onDone}){
  const [appId,setAppId]=useState(""),[endpoint,setEndpoint]=useState(""),[tokenValue,setTokenValue]=useState(""),[sample,setSample]=useState(""),[busy,setBusy]=useState(false),[result,setResult]=useState(null);
  const current=appId||selected?.app_id||apps[0]?.app_id||"";
  useEffect(()=>{
    if(!current||!token)return;
    let alive=true;
    api(`/developer/v1/apps/${current}/data-source`,{token}).then(r=>{
      if(!alive)return;
      setEndpoint(r.endpoint||"");setTokenValue("");
      setSample(r.sample&&Object.keys(r.sample).length?JSON.stringify(r.sample,null,2):"");
      setResult(r.configured?{app_id:current,verdict:r.verdict,checks:r.checks,summary:{},sample:r.sample,checked_at:r.checked_at}:null);
    }).catch(()=>{});
    return ()=>{alive=false};
  },[current,token]);
  async function save(e){
    e.preventDefault();
    if(!current){return}
    setBusy(true);setResult(null);
    let parsed=null;
    if(sample.trim()){
      try{parsed=JSON.parse(sample)}catch(err){window.alert("样本 JSON 格式错误："+err.message);setBusy(false);return}
    }
    try{
      const r=await api(`/developer/v1/apps/${current}/data-source`,{token,method:"POST",body:{endpoint,token:tokenValue,sample:parsed}});
      setResult(r);onDone&&onDone();
    }catch(err){window.alert(err.message)}finally{setBusy(false)}
  }
  return <section className="section-card">
    <div className="section-title"><h2>数据接口（轨2 · 无网页应用上屏）</h2><span className="gh-tag">DATA API</span></div>
    <p className="subtle">应用没有网页也能上屏：提供一个 HTTP 端点，按契约返回 JSON（标题 + 数据项），后端实时渲染成墨水屏卡片。平台请求时会附带 <code>?screen=A</code> / <code>?screen=B</code>，可据此区分双屏内容；无网页的桌面程序（如 codex pulse）走这一轨。</p>
    <form className="gh-form" onSubmit={save}>
      <label>目标应用<select value={current} onChange={e=>{setAppId(e.target.value);setSelected(apps.find(a=>a.app_id===e.target.value)||null)}} required>
        <option value="">{apps.length?"请选择应用":"请先在上方创建应用"}</option>
        {apps.map(a=><option key={a.app_id} value={a.app_id}>{a.name}</option>)}
      </select></label>
      <label>数据端点<input value={endpoint} onChange={e=>setEndpoint(e.target.value)} required placeholder="https://your.app/eink/card"/></label>
      <label>Bearer Token（可选）<input value={tokenValue} onChange={e=>setTokenValue(e.target.value)} placeholder="留空 = 无需鉴权；已保存的 token 不会回显"/></label>
      <label className="gh-wide">样本 JSON（端点暂不可用时供预览/校验，可选）<textarea rows={6} value={sample} onChange={e=>setSample(e.target.value)} placeholder='{"version":1,"title":"API 额度","items":[{"label":"剩余额度","value":"$42.10","emphasis":"red"}]}'/></label>
      <button className="primary" disabled={busy||!current}>{busy?"校验中…":"保存并校验"}</button>
    </form>
    {result&&<DataSourceReport result={result}/>}
  </section>;
}
function DataSourceReport({result}){
  const s=result.summary||{},checks=result.checks||[],verdict=result.verdict||"fail";
  return <div className="gh-report">
    <div className={`gh-verdict ${verdict}`}>
      <b>{verdict==="pass"?"数据接口校验通过":verdict==="warn"?"已通过，有优化建议":"未通过校验"}</b>
      <span>{s.pass||0} 项通过 · {s.warn||0} 项建议 · {s.fail||0} 项不通过{result.checked_at?` · ${result.checked_at}`:""}</span>
      {result.app_id&&<a href={`${BASE}/backend/api/preview?appId=${result.app_id}&persona=DUAL_A`} target="_blank" rel="noreferrer">打开数据卡片预览 →</a>}
    </div>
    <div className="check-list">{checks.map(c=>
      <div key={c.id} className={`check ${c.status}`}>
        <i>{c.status==="pass"?"✓":c.status==="warn"?"!":"×"}</i>
        <div><b>{c.label}</b><p>{c.detail}</p></div>
      </div>
    )}</div>
  </div>;
}
function DevicePreviewModal({appId,onClose}){
  if(!appId)return null;
  const q=`appId=${encodeURIComponent(appId)}&w=400&h=300`;
  const a=`${BASE}/backend/api/preview?${q}&persona=DUAL_A`;
  const b=`${BASE}/backend/api/preview?${q}&persona=DUAL_B&colors=4`;
  return <div className="device-preview-mask" onClick={onClose}><div className="device-preview" onClick={e=>e.stopPropagation()}>
    <div className="dp-head"><b>设备出图预览</b><span>与固件同源渲染 · 所见即设备真实显示</span><button onClick={onClose} aria-label="关闭">×</button></div>
    <div className="dp-body">
      <figure><img src={a} alt="A 屏预览"/><figcaption>A 屏 · 黑白 400×300</figcaption></figure>
      <figure><img src={b} alt="B 屏预览"/><figcaption>B 屏 · 四色 400×300</figcaption></figure>
    </div>
    <p className="dp-note">当前为「双屏适配UI」轨：设备显示应用海报卡（应用名 + 简介 + 入口二维码），用户扫码在手机打开完整交互。若应用无网页（如桌面程序），二维码将无效——需改用「数据接口」轨才能在屏上出真实内容。</p>
  </div></div>;
}
function ModelConfig({apps,selected,setSelected,onSave}){return <section className="section-card model-card"><h2>为终端用户接入 AI 内容生成</h2><p>模型仅在所选应用内使用。API Key 在服务端加密保存，浏览器和查询接口都不会回显。</p><form onSubmit={onSave}><label>应用<select value={selected?.app_id||""} onChange={e=>setSelected(apps.find(a=>a.app_id===e.target.value))} required><option value="">请选择应用</option>{apps.map(a=><option key={a.app_id} value={a.app_id}>{a.name}</option>)}</select></label><div className="form-two"><label>服务商<select name="provider"><option value="deepseek">DeepSeek</option><option value="qwen">通义千问</option><option value="doubao">豆包</option><option value="openai-compatible">OpenAI Compatible</option></select></label><label>模型<input name="model" required defaultValue="deepseek-chat"/></label></div><label>Base URL<input name="base_url" placeholder="使用服务商默认地址可留空"/></label><label>API Key<input name="api_key" type="password" placeholder="留空则保留已配置密钥"/></label><label>内容生成约束<textarea name="system_prompt" rows="5" defaultValue="生成适合电子墨水屏的简洁、准确内容；不得生成违法、有害、色情、暴力、侵权或误导信息。"/></label><button className="primary">加密保存配置</button></form></section>}
function Policy(){return <div className="policy-grid"><section className="section-card"><span className="eyebrow">AGREEMENT v2026-10-23</span><h2>开发者服务协议摘要</h2><p>开发者对提交的代码、素材和数据拥有合法权利；不得绕过沙箱、越权访问设备或收集非必要个人信息；上线版本须经平台审核，平台保留暂停、下架及追责权利。</p><p>沙箱凭据不得用于生产设备。任何生产发布、版本升级和下架操作均仅由平台管理员执行。</p></section><section className="section-card danger-card"><h2>禁止内容与行为</h2><ul><li>政治有害、危害国家安全或破坏社会稳定的信息</li><li>色情低俗、未成年人不良、暴力恐怖或违法犯罪内容</li><li>歧视仇恨、骚扰欺凌、虚假欺诈及危险健康建议</li><li>侵犯著作权、商标权、隐私权或其他第三方权益</li><li>恶意刷新屏幕、损害硬件寿命或绕过平台安全控制</li></ul></section><section className="section-card"><h2>上线流程</h2><div className="flow"><b>01 沙箱创建</b><i>→</i><b>02 模拟器联调</b><i>→</i><b>03 提交审核</b><i>→</i><b>04 管理员发布</b></div></section></div>}
const GUIDE_STEPS=[{n:"01",title:"提交与审核上线",color:"#2b6cb0",desc:["开发者在「创作者中心」创建沙箱应用、从 GitHub 导入，","提交审核；运营后台审核通过后自动登记到","「已上线应用」内容库。"],action:"去创建 / 提交"},{n:"02",title:"已上线应用（内容源）",color:"#2f855a",desc:["审核通过的应用进入内容库，每个应用带 scene","标签（如 office）。内容配置后台按场景分类","展示已上线应用。"],action:"去审核 / 已上线"},{n:"03",title:"场景中心配置 A/B 屏",color:"#975a16",desc:["在内容配置后台选场景，把已上线应用","「设为 A 屏 / B 屏」。保存即下发，","设备下次唤醒自动拉取。"],action:"去场景中心配置"},{n:"04",title:"设备出图",color:"#6b46c1",desc:["设备唤醒后从后端拉取双屏图，墨水屏显示","应用卡片（应用名 + 简介 + 二维码，扫码在","手机打开完整交互）。"],action:null}];
function ConfigGuide({onStep}){const cardX=60,cardW=520,cardH=150,startY=96,gap=40;const arrows=["审核通过自动登记","按 scene 标签筛选","保存即下发"];return <div className="config-guide"><svg viewBox="0 0 640 858" width="100%" role="img" aria-label="配置引导流程图"><text x={320} y={46} textAnchor="middle" fontSize={22} fontWeight={700} fill="#1f2933">小悟双屏 · 配置引导</text><text x={320} y={72} textAnchor="middle" fontSize={13} fill="#6b7280">从提交应用到墨水屏出图，四步完成</text>{GUIDE_STEPS.map((s,i)=>{const y=startY+i*(cardH+gap);const clickable=!!s.action;return <g key={i} className={clickable?"cg-step cg-click":"cg-step"} onClick={clickable?()=>onStep(i):undefined}><rect x={cardX} y={y} width={cardW} height={cardH} rx={16} fill="#ffffff" stroke="#e3e8ee" strokeWidth={1.5}/><circle cx={cardX+40} cy={y+44} r={26} fill={s.color}/><text x={cardX+40} y={y+51} textAnchor="middle" fontSize={18} fontWeight={700} fill="#fff">{s.n}</text><text x={cardX+86} y={y+40} fontSize={18} fontWeight={700} fill="#1f2933">{s.title}</text>{s.desc.map((line,j)=><text key={j} x={cardX+86} y={y+70+j*23} fontSize={13.5} fill="#55606b">{line}</text>)}{s.action&&<text x={cardX+cardW-18} y={y+cardH-18} textAnchor="end" fontSize={13} fontWeight={600} fill={s.color}>▶ {s.action}</text>}</g>})}{arrows.map((label,i)=>{const top=startY+i*(cardH+gap)+cardH;const bottom=startY+(i+1)*(cardH+gap);const mid=(top+bottom)/2;return <g key={i}><line x1={320} y1={top+4} x2={320} y2={bottom-10} stroke="#9aa5b1" strokeWidth={2}/><polygon points={`${320-6},${bottom-18} ${320+6},${bottom-18} 320,${bottom-6}`} fill="#9aa5b1"/><rect x={320-74} y={mid-12} width={148} height={22} rx={11} fill="#ffffff" stroke="#e3e8ee"/><text x={320} y={mid+3} textAnchor="middle" fontSize={12} fill="#6b7280">{label}</text></g>})}<text x={320} y={842} textAnchor="middle" fontSize={12} fill="#9aa5b1">提示：场景中心配置时会从「已上线应用」按场景筛选可添加的内容。</text></svg></div>}
