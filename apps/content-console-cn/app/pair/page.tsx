"use client";

import { useCallback, useEffect, useState } from "react";
import Link from "next/link";
import { AlertTriangle, Check, KeyRound, LoaderCircle, ShieldCheck, Trash2, X } from "lucide-react";
import { api, BASE_PATH, getToken } from "@/lib/api";

type PairScope = { key: string; text: string };
type PairDevice = { device_id: string; nickname?: string; model?: string };
type PairInfo = {
  code: string;
  status: "pending" | "approved" | "denied" | "expired";
  expires_at: string;
  app: { app_id: string; name: string };
  label: string;
  scopes: PairScope[];
  devices: PairDevice[];
};
type Grant = {
  grant_id: string;
  app_id: string;
  app_name?: string;
  device_id: string;
  label: string;
  status: string;
  created_at: string;
  scope_text: string[];
};

const STATUS_TEXT: Record<string, string> = {
  pending: "等待你的确认",
  approved: "该绑定码已被使用",
  denied: "该绑定码已被拒绝",
  expired: "绑定码已过期",
};

function deviceName(d?: PairDevice) {
  if (!d) return "";
  return d.nickname || d.device_id;
}

export default function PairPage() {
  const [loggedIn, setLoggedIn] = useState(false);
  const [code, setCode] = useState("");
  const [info, setInfo] = useState<PairInfo | null>(null);
  const [deviceId, setDeviceId] = useState("");
  const [grants, setGrants] = useState<Grant[]>([]);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");

  const loadGrants = useCallback(async () => {
    try {
      setGrants(await api<Grant[]>("/api/v1/mobile/grants"));
    } catch {
      /* 未登录时静默，主流程会提示 */
    }
  }, []);

  const lookup = useCallback(async (raw: string) => {
    const key = raw.replace(/[^0-9A-Za-z]/g, "").toUpperCase();
    if (key.length < 6) {
      setError("请输入 6 位绑定码");
      setInfo(null);
      return;
    }
    setBusy(true);
    setError("");
    try {
      const data = await api<PairInfo>(`/api/v1/mobile/pairing/${key}`);
      setInfo(data);
      setCode(key);
      setDeviceId(data.devices?.[0]?.device_id || "");
    } catch (e) {
      setInfo(null);
      setError(e instanceof Error ? e.message : "查询失败");
    } finally {
      setBusy(false);
    }
  }, []);

  useEffect(() => {
    if (!getToken()) return;
    setLoggedIn(true);
    loadGrants();
    const fromUrl = new URLSearchParams(window.location.search).get("code") || "";
    if (fromUrl) {
      setCode(fromUrl.toUpperCase());
      lookup(fromUrl);
    }
  }, [loadGrants, lookup]);

  async function confirm() {
    if (!info) return;
    if (!deviceId) {
      setError("请先选择要授权展示的设备");
      return;
    }
    setBusy(true);
    setError("");
    try {
      await api(`/api/v1/mobile/pairing/${info.code}/confirm`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ device_id: deviceId }),
      });
      setNotice(`已允许「${info.app.name}」向 ${deviceName(info.devices.find((d) => d.device_id === deviceId))} 推送内容`);
      setInfo({ ...info, status: "approved" });
      await loadGrants();
    } catch (e) {
      setError(e instanceof Error ? e.message : "授权失败");
    } finally {
      setBusy(false);
    }
  }

  async function deny() {
    if (!info) return;
    setBusy(true);
    setError("");
    try {
      await api(`/api/v1/mobile/pairing/${info.code}/deny`, { method: "POST" });
      setInfo({ ...info, status: "denied" });
      setNotice("已拒绝本次接入申请");
    } catch (e) {
      setError(e instanceof Error ? e.message : "操作失败");
    } finally {
      setBusy(false);
    }
  }

  async function revoke(grant: Grant) {
    setBusy(true);
    setError("");
    try {
      await api(`/api/v1/mobile/grants/${grant.grant_id}`, { method: "DELETE" });
      setNotice(`已撤销「${grant.app_name || grant.app_id}」的授权，其后续推送将立即被拒绝`);
      await loadGrants();
    } catch (e) {
      setError(e instanceof Error ? e.message : "撤销失败");
    } finally {
      setBusy(false);
    }
  }

  if (!loggedIn) {
    return (
      <div className="pair-page">
        <div className="pair-box">
          <div className="pair-brand">
            <img src={`${BASE_PATH}/logo.png`} alt="" />
            <div>
              <b>森穹小悟</b>
              <span>DEVICE AUTHORIZATION</span>
            </div>
          </div>
          <h1>需要先登录</h1>
          <p className="muted">
            授权必须由设备主人本人确认。请先登录内容后台，再打开本页完成授权。
          </p>
          <Link className="button primary full" href="/">
            去登录内容后台
          </Link>
        </div>
      </div>
    );
  }

  return (
    <div className="pair-page">
      <div className="pair-wrap">
        <div className="pair-brand">
          <img src={`${BASE_PATH}/logo.png`} alt="" />
          <div>
            <b>森穹小悟</b>
            <span>DEVICE AUTHORIZATION</span>
          </div>
          <Link className="pair-back" href="/">
            返回内容后台
          </Link>
        </div>

        <div className="pair-hero">
          <span className="kicker">AUTHORIZATION</span>
          <h1>授权管理</h1>
          <p>
            第三方应用通过绑定码申请向你的设备推送内容。只有你本人点击「允许」后它才能推送，
            你随时可以撤销。平台不会获取你的任何账号密码或大模型凭证。
          </p>
        </div>

        <section className="card-pad">
          <h3 className="pair-h3">输入绑定码</h3>
          <p className="muted pair-sub">绑定码由应用显示给你（6 位，10 分钟内有效），输入后即可查看申请内容。</p>
          <form
            className="pair-form"
            onSubmit={(e) => {
              e.preventDefault();
              lookup(code);
            }}
          >
            <input
              className="pair-code-input"
              value={code}
              onChange={(e) => setCode(e.target.value.toUpperCase())}
              placeholder="K7M-2QP"
              maxLength={8}
              autoComplete="off"
            />
            <button className="button primary" disabled={busy}>
              {busy ? <LoaderCircle size={14} className="spin" /> : <KeyRound size={14} />}
              查询
            </button>
          </form>
        </section>

        {error ? <p className="form-error pair-error">{error}</p> : null}
        {notice ? (
          <p className="form-success pair-notice">
            <Check size={13} /> {notice}
          </p>
        ) : null}

        {info ? (
          <section className="card-pad pair-request">
            <div className="pair-request-head">
              <div className="pair-app-mark">{(info.app.name || "A").slice(0, 1)}</div>
              <div>
                <b>{info.app.name}</b>
                <small>
                  申请向你的设备推送内容 · {info.label || "未命名用途"}
                </small>
              </div>
              <span className={`pair-status ${info.status}`}>{STATUS_TEXT[info.status] || info.status}</span>
            </div>

            <div className="pair-scopes">
              <span className="pair-label">申请权限</span>
              {info.scopes.length ? (
                info.scopes.map((s) => (
                  <div key={s.key} className="pair-scope">
                    <ShieldCheck size={13} />
                    <span>{s.text}</span>
                    <code>{s.key}</code>
                  </div>
                ))
              ) : (
                <p className="muted">该应用未申请任何权限</p>
              )}
            </div>

            {info.status === "pending" ? (
              <>
                <div className="pair-scopes">
                  <span className="pair-label">授权给哪台设备</span>
                  {info.devices.length ? (
                    <div className="pair-devices">
                      {info.devices.map((d) => (
                        <button
                          key={d.device_id}
                          type="button"
                          className={`pair-device ${deviceId === d.device_id ? "on" : ""}`}
                          onClick={() => setDeviceId(d.device_id)}
                        >
                          <i />
                          <b>{deviceName(d)}</b>
                          <small>{d.device_id}</small>
                          {deviceId === d.device_id ? <Check size={14} /> : null}
                        </button>
                      ))}
                    </div>
                  ) : (
                    <div className="pair-warn">
                      <AlertTriangle size={14} />
                      你还没有绑定任何设备，请先在内容后台「设备管理」完成绑定。
                    </div>
                  )}
                </div>

                <div className="pair-actions">
                  <button className="button primary" disabled={busy || !info.devices.length} onClick={confirm}>
                    <Check size={14} /> 允许并授权
                  </button>
                  <button className="button ghost" disabled={busy} onClick={deny}>
                    <X size={14} /> 拒绝
                  </button>
                </div>
                <p className="pair-legal">
                  点击「允许」即表示你同意该应用按上方权限向所选设备推送内容。授权记录可在下方随时撤销。
                </p>
              </>
            ) : (
              <div className="pair-warn">
                <AlertTriangle size={14} />
                {STATUS_TEXT[info.status] || info.status}，请向应用索取新的绑定码。
              </div>
            )}
          </section>
        ) : null}

        <section className="card-pad">
          <h3 className="pair-h3">已授权的应用</h3>
          <p className="muted pair-sub">撤销后该应用的下一次推送会立即被平台拒绝（约 1 秒内生效）。</p>
          {grants.length ? (
            <div className="pair-grants">
              {grants.map((g) => (
                <div key={g.grant_id} className={`pair-grant ${g.status}`}>
                  <div>
                    <b>{g.app_name || g.app_id}</b>
                    <small>
                      {g.label || "推送内容"} · 设备 {g.device_id}
                    </small>
                    <em>{(g.scope_text || []).join("、") || "—"}</em>
                  </div>
                  {g.status === "active" ? (
                    <button className="button danger sm" disabled={busy} onClick={() => revoke(g)}>
                      <Trash2 size={12} /> 撤销
                    </button>
                  ) : (
                    <span className="pair-revoked">已撤销</span>
                  )}
                </div>
              ))}
            </div>
          ) : (
            <p className="muted pair-empty">暂无授权记录。应用申请接入后，会先出现在这里等你确认。</p>
          )}
        </section>
      </div>
    </div>
  );
}
