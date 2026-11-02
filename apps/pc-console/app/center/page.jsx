"use client";

import { useEffect, useState } from "react";
import Link from "next/link";

// 运营总览现在挂在 /center,API 走同域 /backend(由 next.config 反代到后端 8104)
const BASE = "";

const tabs = [["overview","运营概览"],["creator-accounts","注册用户"],["developer-submissions","贡献应用"],["published-apps","已上线应用"],["devices","设备"],["tasks","内容任务"],["templates","内容模板"],["plugins","插件"],["developer-apps","API 应用"],["webhook-outbox","Webhook 队列"],["audit-logs","审计日志"]];
function value(v) { if (v == null || v === "") return "—"; return typeof v === "object" ? JSON.stringify(v, null, 2) : String(v); }

export default function OperationsConsole() {
  const [tab, setTab] = useState("overview");
  const [token, setToken] = useState("");
  const [data, setData] = useState([]);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(false);
  const [appName, setAppName] = useState("行业接入应用");
  const [createdToken, setCreatedToken] = useState("");
  const authHeaders = token ? { Authorization: `Bearer ${token}` } : {};
  async function load(next = tab) {
    setLoading(true); setError("");
    try {
      const res = await fetch(`${BASE}/backend/admin/v1/${next}`, { headers: authHeaders, cache: "no-store" });
      if (!res.ok) throw new Error(res.status === 401 ? "登录已过期，请重新登录" : `请求失败 HTTP ${res.status}`);
      setData(await res.json());
    } catch (e) { setError(e instanceof Error ? e.message : "请求失败"); } finally { setLoading(false); }
  }
  useEffect(() => { setToken(localStorage.getItem("xiaowu-admin-token")||""); }, []);
  useEffect(() => { if(token) void load(tab); }, [tab,token]); // eslint-disable-line react-hooks/exhaustive-deps
  async function login(e){e.preventDefault();setError("");const body=Object.fromEntries(new FormData(e.currentTarget));const res=await fetch(`${BASE}/backend/admin/v1/auth/login`,{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(body)});const result=await res.json().catch(()=>({}));if(!res.ok)return setError(result.detail||"登录失败");localStorage.setItem("xiaowu-admin-token",result.token);setToken(result.token)}
  function logout(){localStorage.removeItem("xiaowu-admin-token");setToken("");setData([])}
  async function createDeveloperApp() {
    setError(""); setCreatedToken("");
    const res = await fetch(`${BASE}/backend/admin/v1/developer-apps`, { method:"POST", headers:{...authHeaders,"Content-Type":"application/json"}, body:JSON.stringify({name:appName,scopes:["device.content.write","device.read","device.event.read","webhook.manage"]}) });
    if (!res.ok) return setError(`创建失败 HTTP ${res.status}`);
    const result = await res.json(); setCreatedToken(result.token); await load("developer-apps");
  }
  if(!token)return <main className="admin-login-page"><section className="admin-login-card"><img src={`${BASE}/logo.png`} alt="森穹小悟"/><small>PLATFORM ADMINISTRATION</small><h1>小悟众创管理后台</h1><p>查看注册用户、贡献应用与审核状态。</p><form onSubmit={login}><label>管理员账号<input name="username" autoComplete="username" required/></label><label>密码<input name="password" type="password" autoComplete="current-password" required/></label>{error&&<div className="error">{error}</div>}<button>登录管理后台</button></form></section></main>;
  const overview = !Array.isArray(data) ? data : {}, rows = Array.isArray(data) ? data : [];
  return <main className="admin-app"><aside><div className="brand" style={{display:'flex',alignItems:'center',gap:10}}><img src={`${BASE}/logo.png`} alt="森穹小悟" width={34} height={34} style={{borderRadius:9,objectFit:'contain',background:'#fff',border:'1px solid #2b4036'}}/><div><span>森穹小悟</span><b>S3 OPERATIONS</b></div></div><nav><a href="/admin" className="cc-entry">双屏内容配置 →</a><Link href="/" className="dev-entry">创作者门户 →</Link>{tabs.map(([id,label])=><button className={tab===id?"active":""} key={id} onClick={()=>setTab(id)}>{label}</button>)}</nav><p>运营能力仅保留在 PC 端。小程序只负责用户内容管理。</p></aside><section>
    <header><div><small>OPEN PLATFORM / ADMIN</small><h1>{tabs.find(x=>x[0]===tab)?.[1]}</h1></div><div className="auth"><button onClick={()=>void load()}>刷新</button><button className="logout-admin" onClick={logout}>退出登录</button></div></header>
    {error&&<div className="error">{error}</div>}{loading&&<div className="loading">正在读取数据…</div>}
    {tab==="overview"&&!loading&&<div className="metrics"><Metric label="设备总数" value={overview.devices}/><Metric label="已有心跳" value={overview.seen_devices}/><Metric label="待下发任务" value={overview.queued_tasks}/><Metric label="已显示内容" value={overview.displayed_tasks}/></div>}
    {tab==="developer-apps"&&!loading&&<div className="app-create"><input value={appName} onChange={e=>setAppName(e.target.value)} placeholder="应用名称"/><button onClick={()=>void createDeveloperApp()}>创建全权限测试应用</button>{createdToken&&<div><b>请立即保存，仅显示一次：</b><code>{createdToken}</code></div>}</div>}
    {tab==="developer-submissions"&&!loading&&<ReviewTable rows={rows} token={token} reload={()=>load("developer-submissions")} setError={setError}/>}
    {tab==="published-apps"&&!loading&&<PublishedAppTable rows={rows} token={token} reload={()=>load("published-apps")} setError={setError}/>}
    {tab!=="overview"&&tab!=="developer-submissions"&&tab!=="published-apps"&&!loading&&<DataTable rows={rows}/>}</section></main>;
}
function Metric({label,value:v}) { return <article className="metric"><span>{label}</span><b>{value(v)}</b></article>; }
function DataTable({rows}) { if (!rows.length) return <div className="empty">暂无数据</div>; const keys=Object.keys(rows[0]); return <div className="table-wrap"><table><thead><tr>{keys.map(k=><th key={k}>{k}</th>)}</tr></thead><tbody>{rows.map((row,i)=><tr key={i}>{keys.map(k=><td key={k}><pre>{value(row[k])}</pre></td>)}</tr>)}</tbody></table></div>; }
function ReviewTable({rows,token,reload,setError}) {
  const [preview,setPreview]=useState(null);
  async function decide(id,decision){const note=window.prompt(decision==="approve"?"发布备注（可留空）":"驳回原因")||"";const res=await fetch(`${BASE}/backend/admin/v1/developer-submissions/${id}/decision`,{method:"POST",headers:{Authorization:`Bearer ${token}`,"Content-Type":"application/json"},body:JSON.stringify({decision,note})});if(!res.ok){setError(`处理失败 HTTP ${res.status}`);return}reload()}
  if(!rows.length)return <div className="empty">暂无开发者提交</div>;
  return <><div className="review-list">{rows.map(a=><article key={a.app_id}><div><span className={`status ${a.status}`}>{a.status}</span><h3>{a.name}</h3><p>{a.description||"暂无说明"}</p><code>{a.app_id} · {a.scene} · v{a.version}</code>{a.track&&<div className="app-track">提交轨：{a.track==="data"?"数据接口":a.track==="static"?"双屏适配UI":"未配置"}{a.track==="data"&&a.data_source?` · ${a.data_source.verdict==="pass"?"已通过":a.data_source.verdict==="warn"?"有建议":"未通过"}`:""}</div>}</div><div>{a.status==="in_review"?<><button onClick={()=>decide(a.app_id,"reject")}>驳回</button><button className="approve" onClick={()=>decide(a.app_id,"approve")}>审核并上线</button></>:<small>{a.review_note||"—"}</small>}<button className="ghost" onClick={()=>setPreview(a.app_id)}>设备出图预览</button></div></article>)}</div>{preview&&<DevicePreviewModal appId={preview} onClose={()=>setPreview(null)}/>}</>;
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
function PublishedAppTable({rows,token,reload,setError}) { async function toggle(id,enabled){const res=await fetch(`${BASE}/backend/admin/v1/published-apps/${id}/toggle`,{method:"POST",headers:{Authorization:`Bearer ${token}`,"Content-Type":"application/json"},body:JSON.stringify({enabled})});if(!res.ok){setError(`操作失败 HTTP ${res.status}`);return}reload()} if(!rows.length)return <div className="empty">暂无已上线应用。开发者提交的应用经「贡献应用」审核通过后，会自动登记到这里。</div>;return <div className="review-list">{rows.map(a=><article key={a.app_id}><div><span className={`status ${a.enabled? "published":"rejected"}`}>{a.enabled? "已启用":"已下线"}</span><h3>{a.name}</h3><p>{a.description||"暂无说明"}</p><code>{a.app_id} · {a.scene} · {a.preview_url}</code></div><div>{a.enabled?<button className="reject" onClick={()=>toggle(a.app_id,false)}>下线</button>:<button className="approve" onClick={()=>toggle(a.app_id,true)}>重新启用</button>}</div></article>)}</div> }
