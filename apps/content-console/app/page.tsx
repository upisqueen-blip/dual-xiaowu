"use client";

import { useEffect, useMemo, useRef, useState } from "react";
import {
  Bell, BookOpen, Boxes, CalendarDays, Check, ChevronDown, CircleUserRound, Cloud, CloudRain, CloudSnow, CloudSun, Sun, Cpu,   Home,
  History, KeyRound, LayoutDashboard, Link2, ListChecks, LoaderCircle, LogOut, MonitorCog, MoreHorizontal,
  NotebookPen, Plus, RefreshCw, Save, ScanText, Settings2, ShieldCheck, Sparkles, StickyNote, Trash2, UserCog, Users,
  Image as ImageIcon,
  WalletCards, Truck, Wifi, X,
} from "lucide-react";
import { api, clearToken, getToken, setToken } from "@/lib/api";

type SceneKey = string;
type NavKey = "dashboard" | "scenes" | "screens" | "content" | "apps" | "devices" | "members" | "auth" | "profile" | "settings";
type ScreenConfig = { title: string; source: string; refresh: string; modules: string[]; mode?: string; appId?: string };
type DualWeatherPreview = { city: string; today_temp: string; current_temp?: string; today_desc: string; today_code?: number; today_low: string; today_high: string; today_humidity?: string; today_wind_dir?: string; today_wind_level?: string; sunrise?: string; sunset?: string; advice?: string; forecast?: { day: string; date: string; temp_range: string; desc: string; code?: number }[]; feels_like?: string | number | null };
type Scene = { name: string; eyebrow: string; desc: string; icon: string; a: ScreenConfig; b: ScreenConfig };
type User = { user_id: number; username: string };
type Device = { mac: string; nickname?: string; role?: string; status?: string; is_online?: boolean; last_seen?: string };

const presetScenes: Record<string, Scene> = {
  family: {
    name: "家庭", eyebrow: "默认场景", icon: "家", desc: "让每个人都看见今天的重要安排",
    a: { title: "今日家庭提醒", source: "家庭内容管理", refresh: "每 10 分钟", mode: "family", modules: ["天气", "快递与缴费", "家庭待办", "采购清单", "纪念日"] },
    b: { title: "共享日程", source: "家庭日程（内容管理）", refresh: "每 30 分钟", mode: "family", modules: ["月/周视图", "成员标记", "今日重点", "节假日", "下一项安排"] },
  },
  study: {
    name: "学习", eyebrow: "学生与家庭", icon: "学", desc: "课程、作业和目标始终在视线里",
    a: { title: "作业与便签", source: "后台作业/便签", refresh: "每 5 分钟", modules: ["今日作业", "老师便签", "背词任务", "阅读任务", "完成进度"] },
    b: { title: "课程与目标", source: "课程表/倒计时", refresh: "每 15 分钟", mode: "course", modules: ["课程表", "考试倒计时", "当前课程", "下一节课", "自动切换", "相册"] },
  },
  finance: {
    name: "理财", eyebrow: "行情关注", icon: "财", desc: "快速价格与低频趋势各司其职",
    a: { title: "实时行情", source: "行情数据源", refresh: "每 5 分钟", mode: "finance-watch", modules: ["股票自选", "黄金/期货", "当前价格", "涨跌幅", "交易状态"] },
    b: { title: "行情趋势", source: "趋势聚合", refresh: "每 10 分钟", mode: "finance-trend", modules: ["日内趋势", "日 K 简图", "涨跌排行", "资产概览", "市场摘要"] },
  },
  health: {
    name: "养生", eyebrow: "健康管理", icon: "养", desc: "把日常作息、饮水与运动提醒放在眼前",
    a: { title: "今日健康提醒", source: "后台手动配置", refresh: "每 30 分钟", mode: "health", modules: ["饮水提醒", "用药提醒", "作息建议", "今日步数", "舒展活动"] },
    b: { title: "养生计划", source: "后台手动配置", refresh: "每 12 小时", mode: "health-plan", modules: ["今日饮食", "运动安排", "睡眠目标", "健康小贴士"] },
  },
  office: {
    name: "办公", eyebrow: "专注工作", icon: "办", desc: "工作状态与氛围展示的双屏组合",
    a: { title: "工作信息", source: "后台模块组合", refresh: "每 10 分钟", mode: "office", modules: ["Token 余额", "今日调用", "本月费用", "服务状态", "每日资讯"] },
    b: { title: "办公展示", source: "后台相册/TF 卡", refresh: "每 30 分钟", mode: "album", modules: ["团队照片", "企业海报", "产品宣传", "二维码", "会议室状态"] },
  },
  custom: {
    name: "自由组合", eyebrow: "空白画布", icon: "自", desc: "自由编排的双屏画布：可搭建模块，也能直接编排已上线的应用",
    a: { title: "自由组合 A 屏", source: "用户配置", refresh: "自定义", mode: "custom", modules: ["待办", "天气", "便签"] },
    b: { title: "自由组合 B 屏", source: "用户配置", refresh: "自定义", mode: "calendar", modules: ["日程", "相册", "倒计时"] },
  },
};

// 预设场景为不可变基线；scenes 为「预设 + 用户自定义」合并后的可变映射（运行时由后端自定义场景覆盖/补充）。
let scenes: Record<string, Scene> = { ...presetScenes };

const navItems: { key: NavKey; label: string; icon: typeof Home }[] = [
  { key: "dashboard", label: "总览", icon: LayoutDashboard },
  { key: "scenes", label: "场景中心", icon: Sparkles },
  { key: "screens", label: "双屏配置", icon: MonitorCog },
  { key: "content", label: "内容管理", icon: NotebookPen },
  { key: "apps", label: "已上线应用", icon: Boxes },
  { key: "devices", label: "设备管理", icon: Cpu },
  { key: "members", label: "共享成员", icon: Users },
  { key: "auth", label: "老师授权", icon: KeyRound },
  { key: "profile", label: "个人信息", icon: UserCog },
  { key: "settings", label: "系统配置", icon: Settings2 },
];

export default function DualConsole() {
  const [nav, setNav] = useState<NavKey>("dashboard");
  const [sceneKey, setSceneKey] = useState<SceneKey>("family");
  const [deviceMac, setDeviceMac] = useState("");
  const [devices, setDevices] = useState<Device[]>([]);
  const [user, setUser] = useState<User | null>(null);
  const [loading, setLoading] = useState(true);
  const [authOpen, setAuthOpen] = useState(false);
  const [pairOpen, setPairOpen] = useState(false);
  const [toast, setToast] = useState("");
  const [savedAt, setSavedAt] = useState("");
  const [sceneTick, setSceneTick] = useState(0);
  const [customSceneKeys, setCustomSceneKeys] = useState<string[]>([]);
  const [config, setConfig] = useState<Record<SceneKey, { a: ScreenConfig; b: ScreenConfig }>>(() =>
    Object.fromEntries(Object.entries(scenes).map(([k, v]) => [k, { a: v.a, b: v.b }])) as Record<SceneKey, { a: ScreenConfig; b: ScreenConfig }>,
  );
  const [content, setContent] = useState<ContentState>(() => makeDefaultContent());
  // 屏幕规格（A/B 屏尺寸一致；色彩为 B 屏 FPC2 面板能力，需与硬件一致）：
  // size = 4.2" / 5.83"；colors = 2(黑白) / 3(黑白红) / 4(黑白红黄)。
  const [screen, setScreen] = useState<{ size: "4.2" | "5.83"; colors: 2 | 3 | 4 }>({ size: "4.2", colors: 4 });

  const scene = scenes[sceneKey];
  const selectedDevice = devices.find((d) => d.mac === deviceMac) || devices[0];

  useEffect(() => {
    const raw = localStorage.getItem("xiaowu_dual_config");
    if (raw) {
      try {
        const saved = JSON.parse(raw);
        if (saved.sceneKey && scenes[saved.sceneKey as SceneKey]) setSceneKey(saved.sceneKey);
        if (saved.config) setConfig((prev) => ({ ...prev, ...saved.config }));
        if (saved.deviceMac) setDeviceMac(saved.deviceMac);
        if (saved.content) setContent((prev) => ({
          courseTable: saved.content.courseTable && typeof saved.content.courseTable === "object"
            ? { ...prev.courseTable, ...saved.content.courseTable }
            : prev.courseTable,
          notes: Array.isArray(saved.content.notes) ? saved.content.notes : prev.notes,
          homework: Array.isArray(saved.content.homework) ? saved.content.homework : prev.homework,
          reading: Array.isArray(saved.content.reading) ? saved.content.reading : prev.reading,
          album: saved.content.album && Array.isArray(saved.content.album.photos)
            ? saved.content.album
            : prev.album,
          bills: Array.isArray(saved.content.bills) ? saved.content.bills : prev.bills,
          express: Array.isArray(saved.content.express) ? saved.content.express : prev.express,
          familySchedule: Array.isArray(saved.content.familySchedule) ? saved.content.familySchedule : prev.familySchedule,
          familyShopping: Array.isArray(saved.content.familyShopping) ? saved.content.familyShopping : prev.familyShopping,
          familyAnniversaries: Array.isArray(saved.content.familyAnniversaries) ? saved.content.familyAnniversaries : prev.familyAnniversaries,
          familyMembers: Array.isArray(saved.content.familyMembers) ? saved.content.familyMembers.map((member: string | FamilyMember) => typeof member === "string" ? ({ name: member, role: "member" as const }) : ({ name: member.name, role: member.role === "admin" ? "admin" as const : "member" as const })).filter((member: FamilyMember) => !!member.name) : prev.familyMembers,
          weatherCity: typeof saved.content.weatherCity === "string" ? saved.content.weatherCity : prev.weatherCity,
        }));
        if (saved.screen && (saved.screen.size === "4.2" || saved.screen.size === "5.83")) {
          const c = saved.screen.colors;
          setScreen({ size: saved.screen.size, colors: c === 2 || c === 3 || c === 4 ? c : 4 });
        }
      } catch { /* ignore damaged local draft */ }
    }
    void bootstrap();
  }, []);

  async function bootstrap() {
    if (!getToken()) { setLoading(false); return; }
    try {
      const me = await api<User>("/api/auth/me");
      setUser(me);
      await loadDevices();
      await loadUserScenes();
    } catch {
      clearToken();
    } finally { setLoading(false); }
  }

  async function loadDevices() {
    try {
      const result = await api<{ devices?: Device[] }>("/api/user/devices");
      const list = result.devices || [];
      setDevices(list);
      setDeviceMac((current) => current || list[0]?.mac || "");
    } catch (error) { notify((error as Error).message); }
  }

  // 加载当前用户的「自定义场景」（后端持久化），合并进 scenes 映射并补齐 config 条目。
  async function loadUserScenes() {
    if (!getToken()) return;
    try {
      const r = await api<{ scenes: { scene_key: string; name: string; eyebrow: string; desc: string; icon: string }[] }>("/api/scenes");
      const map: Record<string, Scene> = {};
      for (const s of r.scenes || []) {
        const base: Scene = {
          name: s.name, eyebrow: s.eyebrow || "自定义场景", icon: s.icon || "自", desc: s.desc || "",
          a: { title: `${s.name} · A 屏`, source: "用户配置", refresh: "自定义", mode: "custom", modules: [] },
          b: { title: `${s.name} · B 屏`, source: "用户配置", refresh: "自定义", mode: "calendar", modules: [] },
        };
        map[s.scene_key] = base;
      }
      scenes = { ...presetScenes, ...map };
      setCustomSceneKeys(Object.keys(map));
      setConfig((prev) => {
        const next: Record<string, { a: ScreenConfig; b: ScreenConfig }> = { ...prev };
        for (const k of Object.keys(map)) if (!next[k]) next[k] = { a: { ...map[k].a }, b: { ...map[k].b } };
        return next;
      });
      setSceneTick((t) => t + 1);
    } catch (error) { notify((error as Error).message); }
  }

  async function createUserScene(data: { name: string; eyebrow?: string; desc?: string; icon?: string }) {
    const r = await api<{ scene: { scene_key: string; name: string } }>("/api/scenes", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ name: data.name, eyebrow: data.eyebrow || "", desc: data.desc || "", icon: data.icon || "自" }),
    });
    await loadUserScenes();
    notify(`已创建场景「${r.scene.name}」`);
  }

  async function deleteUserScene(key: string) {
    await api(`/api/scenes/${encodeURIComponent(key)}`, { method: "DELETE" });
    await loadUserScenes();
    notify("已删除自定义场景");
  }

  function notify(message: string) {
    setToast(message);
    window.setTimeout(() => setToast(""), 2600);
  }

  function chooseScene(key: SceneKey) {
    setSceneKey(key);
    notify(`已切换到${scenes[key].name}场景，保存后同步到设备`);
  }

  function updateScreen(screen: "a" | "b", patch: Partial<ScreenConfig>) {
    setConfig((all) => ({
      ...all,
      [sceneKey]: { ...all[sceneKey], [screen]: { ...all[sceneKey][screen], ...patch } },
    }));
  }

  function buildPayload() {
    return { version: 1, sceneKey, config, content, screen, deviceMac: selectedDevice?.mac || "", savedAt: new Date().toISOString() };
  }
  function saveConfiguration() {
    localStorage.setItem("xiaowu_dual_config", JSON.stringify(buildPayload()));
    setSavedAt(new Date().toLocaleTimeString("zh-CN", { hour: "2-digit", minute: "2-digit" }));
    notify(selectedDevice ? `已保存 ${selectedDevice.nickname || selectedDevice.mac} 的双屏模式` : "双屏模式已保存为本地模式");
  }
  async function applyToDevice() {
    if (!selectedDevice) { notify("请先在设备管理绑定并选择一台设备"); return; }
    const payload = buildPayload();
    localStorage.setItem("xiaowu_dual_config", JSON.stringify(payload));
    setSavedAt(new Date().toLocaleTimeString("zh-CN", { hour: "2-digit", minute: "2-digit" }));
    try {
      await api(`/api/device/${encodeURIComponent(selectedDevice.mac)}/dual-config`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload) });
      notify(`已保存双屏配置到服务器，设备下次唤醒将自动拉取渲染（${selectedDevice.nickname || selectedDevice.mac}）`);
    } catch (error) {
      const msg = error instanceof Error && error.message ? error.message : "网络错误";
      notify(`下发失败：${msg}（本地模式已保存）`);
    }
  }

  async function assignPublishedApp(screen: "a" | "b", app: { app_id: string; name: string; preview_url: string }) {
    updateScreen(screen, {
      mode: "published-app",
      appId: app.app_id,
      title: app.name,
      source: app.preview_url,
      refresh: "每次唤醒",
      modules: [],
    });
    saveConfiguration();
    notify(`已将「${app.name}」配置到 ${screen.toUpperCase()} 屏，正在下发到设备…`);
    await applyToDevice();
  }

  function logout() {
    clearToken(); setUser(null); setDevices([]); setDeviceMac(""); notify("已退出登录");
  }

  const title = { dashboard: "设备总览", scenes: "场景中心", screens: "双屏配置", content: "内容管理", apps: "已上线应用", devices: "设备管理", members: "共享成员", auth: "老师授权", profile: "个人信息", settings: "系统配置" }[nav];

  return (
    <div className="shell">
      <aside className="sidebar">
        <div className="brand"><div className="brand-mark">悟</div><div><strong>森穹小悟</strong><span>DUAL DISPLAY</span></div></div>
        <div className="nav-label">工作台</div>
        <nav>
          {navItems.map((item) => <button key={item.key} className={nav === item.key ? "active" : ""} onClick={() => setNav(item.key)}><item.icon size={18}/>{item.label}{item.key === "devices" && devices.length > 0 ? <em>{devices.length}</em> : null}</button>)}
        </nav>
        <div className="sidebar-foot">
          <div className="hardware-map"><span><i className="dot-a"/>A 屏 · FPC1</span><small>黑白快刷</small></div>
          <div className="hardware-map"><span><i className="dot-b"/>B 屏 · FPC2</span><small>三色 / 四色</small></div>
          <div className="profile">
            <CircleUserRound size={28}/><div><b>{user?.username || "未登录"}</b><span>{user ? "设备管理员" : "登录后管理设备"}</span></div>
            {user ? <button aria-label="退出" onClick={logout}><LogOut size={16}/></button> : null}
          </div>
        </div>
      </aside>

      <main>
        <header className="topbar">
          <div><span className="crumb">双屏后台 / {title}</span><h1>{title}</h1></div>
          <div className="top-actions">
            {selectedDevice ? <div className="device-select"><span className={selectedDevice.is_online ? "online" : "offline"}/><select value={selectedDevice.mac} onChange={(e) => setDeviceMac(e.target.value)}>{devices.map((d) => <option key={d.mac} value={d.mac}>{d.nickname || "双屏设备"} · {d.mac.slice(-5)}</option>)}</select><ChevronDown size={14}/></div> : null}
            <button className="icon-button" aria-label="通知"><Bell size={18}/></button>
            {user ? <button className="button soft" onClick={() => setPairOpen(true)}><Plus size={16}/>绑定设备</button> : <button className="button primary" onClick={() => setAuthOpen(true)}>登录后台</button>}
          </div>
        </header>

        <div className="content">
          {loading ? <div className="loading"><LoaderCircle className="spin"/>正在连接 小悟AI…</div> : null}
          {!loading && nav === "dashboard" ? <Dashboard user={user} devices={devices} sceneKey={sceneKey} selectedDevice={selectedDevice} setNav={setNav} openAuth={() => setAuthOpen(true)} openPair={() => setPairOpen(true)} /> : null}
          {!loading && nav === "scenes" ? <SceneCenter selected={sceneKey} choose={chooseScene} openConfig={() => setNav("screens")} onAssign={assignPublishedApp} hasDevice={!!selectedDevice} notify={notify} customSceneKeys={customSceneKeys} onCreate={createUserScene} onDelete={deleteUserScene} loggedIn={!!user} /> : null}
          {!loading && nav === "screens" ? <ScreenEditor scene={scene} sceneKey={sceneKey} value={config[sceneKey]} update={updateScreen} save={saveConfiguration} savedAt={savedAt} content={content} apply={applyToDevice} hasDevice={!!selectedDevice} deviceMac={selectedDevice?.mac || ""} screen={screen} setScreen={setScreen} /> : null}
          {!loading && nav === "content" ? <ContentStudio content={content} setContent={setContent} save={saveConfiguration} savedAt={savedAt} notify={notify} apply={applyToDevice} hasDevice={!!selectedDevice} deviceMac={selectedDevice?.mac || ""} screen={screen} sceneKey={sceneKey} aMode={config[sceneKey].a.mode || "notes"} bMode={config[sceneKey].b.mode || "course"} /> : null}
          {!loading && nav === "apps" ? <PublishedApps onAssign={assignPublishedApp} hasDevice={!!selectedDevice} notify={notify} /> : null}
          {!loading && nav === "devices" ? <DeviceManager devices={devices} user={user} refresh={loadDevices} openAuth={() => setAuthOpen(true)} openPair={() => setPairOpen(true)} notify={notify} choose={(mac) => { setDeviceMac(mac); setNav("screens"); }} /> : null}
          {!loading && nav === "members" ? <MembersPanel device={selectedDevice} user={user} notify={notify} /> : null}
          {!loading && nav === "auth" ? <TeacherAuthPanel device={selectedDevice} user={user} notify={notify} /> : null}
          {!loading && nav === "profile" ? <ProfilePanel user={user} notify={notify} content={content} setContent={setContent} save={saveConfiguration} apply={applyToDevice} hasDevice={!!selectedDevice} /> : null}
          {!loading && nav === "settings" ? <ServiceConfigPanel user={user} notify={notify} /> : null}
        </div>
      </main>

      {authOpen ? <AuthModal onClose={() => setAuthOpen(false)} onSuccess={async (nextUser) => { setUser(nextUser); setAuthOpen(false); await loadDevices(); notify(`欢迎回来，${nextUser.username}`); }} /> : null}
      {pairOpen ? <PairModal onClose={() => setPairOpen(false)} onSuccess={async () => { setPairOpen(false); await loadDevices(); notify("设备绑定成功"); }} /> : null}
      {toast ? <div className="toast"><Check size={16}/>{toast}</div> : null}
    </div>
  );
}

function Dashboard({ user, devices, sceneKey, selectedDevice, setNav, openAuth, openPair }: { user: User | null; devices: Device[]; sceneKey: SceneKey; selectedDevice?: Device; setNav: (v: NavKey) => void; openAuth: () => void; openPair: () => void }) {
  const activeScene = scenes[sceneKey];
  return <>
    <section className="welcome">
      <div><span className="kicker">GOOD AFTERNOON</span><h2>{user ? `${user.username}，今天想让双屏显示什么？` : "把重要的信息，安静地放在眼前。"}</h2><p>{user ? "场景模板负责开始，A/B 屏仍可独立微调。" : "登录现有 小悟AI 账号，继续管理设备、绑定关系与双屏场景。"}</p></div>
      <div className="welcome-actions">{user ? <><button className="button primary" onClick={() => setNav("scenes")}><Sparkles size={16}/>选择场景</button><button className="button light" onClick={openPair}><Link2 size={16}/>绑定新设备</button></> : <button className="button primary" onClick={openAuth}>登录 小悟AI 账号</button>}</div>
    </section>
    <section className="stats">
      <Stat icon={<Cpu/>} label="我的设备" value={String(devices.length)} note={devices.length ? "已接入当前账号" : "等待绑定"}/>
      <Stat icon={<Wifi/>} label="当前状态" value={selectedDevice?.is_online ? "在线" : "离线"} note={selectedDevice ? selectedDevice.mac : "尚未选择设备"}/>
      <Stat icon={<Sparkles/>} label="当前场景" value={activeScene.name} note={`${activeScene.a.title} + ${activeScene.b.title}`}/>
      <Stat icon={<Cloud/>} label="配置同步" value={selectedDevice?.is_online ? "可同步" : "待上线"} note="配置草稿安全保留"/>
    </section>
    <div className="section-heading"><div><span>当前方案</span><h3>{activeScene.name}场景</h3></div><button className="text-button" onClick={() => setNav("scenes")}>更换场景 →</button></div>
    <div className="dual-overview"><ScreenSummary kind="a" config={activeScene.a}/><div className="flow"><span>场景协同</span><i/><span>独立刷新</span></div><ScreenSummary kind="b" config={activeScene.b}/></div>
  </>;
}

function Stat({ icon, label, value, note }: { icon: React.ReactNode; label: string; value: string; note: string }) {
  return <div className="stat"><div className="stat-icon">{icon}</div><div><span>{label}</span><strong>{value}</strong><small>{note}</small></div></div>;
}

function SceneCenter({ selected, choose, openConfig, onAssign, hasDevice, notify, customSceneKeys, onCreate, onDelete, loggedIn }: {
  selected: SceneKey; choose: (k: SceneKey) => void; openConfig: () => void;
  onAssign: (s: "a" | "b", app: { app_id: string; name: string; preview_url: string }) => Promise<void>;
  hasDevice: boolean; notify: (m: string) => void; customSceneKeys: string[];
  onCreate: (data: { name: string; eyebrow?: string; desc?: string; icon?: string }) => Promise<void>;
  onDelete: (key: string) => Promise<void>; loggedIn: boolean;
}) {
  const [apps, setApps] = useState<{ app_id: string; name: string; description: string; scene: string; preview_url: string }[]>([]);
  const [appErr, setAppErr] = useState("");
  const [busy, setBusy] = useState("");
  const [creating, setCreating] = useState(false);
  const [form, setForm] = useState({ name: "", eyebrow: "", desc: "", icon: "自" });
  useEffect(() => {
    api<{ apps: { app_id: string; name: string; description: string; scene: string; preview_url: string }[] }>("/api/published-apps")
      .then((r) => setApps(r.apps || []))
      .catch((e) => setAppErr((e as Error).message));
  }, []);
  const current = scenes[selected];
  const isCustom = customSceneKeys.includes(selected);
  // 用户自定义场景与预设「自由组合」(custom) 画布都展示全部已上线应用，供手动编排；
  // 其余预设场景则按应用提交时归属的 scene 标签过滤。
  const showAllApps = isCustom || selected === "custom";
  const sceneApps = showAllApps ? apps : apps.filter((a) => a.scene === selected);
  const assign = (s: "a" | "b", app: { app_id: string; name: string; preview_url: string }) => {
    setBusy(app.app_id + s);
    onAssign(s, app).finally(() => setBusy(""));
  };
  return <>
    <div className="page-intro"><div><span className="kicker">SCENE LIBRARY</span><h2>先选生活，再配屏幕</h2><p>每个场景都是一套可修改的双屏起点，切换不会删除你的自定义内容。已上线的应用可按场景归类并配置到 A/B 屏。</p></div></div>
    <div className="scene-grid">
      {(Object.entries(scenes) as [SceneKey, Scene][]).map(([key, item]) => (
        <div key={key} className={`scene-cell ${selected === key ? "selected" : ""}`}>
          <button onClick={() => choose(key)} className="scene-card">
            <div className="scene-top"><span className="scene-icon">{item.icon}</span><span className="scene-eyebrow">{item.eyebrow}</span>{selected === key ? <i><Check size={13}/></i> : null}</div>
            <h3>{item.name}场景</h3><p>{item.desc}</p>
            <div className="scene-screens"><span><b>A</b>{item.a.title}</span><span><b>B</b>{item.b.title}</span></div>
          </button>
          {isCustom && key === selected && loggedIn ? (
            <button className="scene-del" title="删除该自定义场景" onClick={() => { if (window.confirm(`确定删除自定义场景「${item.name}」？已配置的内容会一并失效。`)) onDelete(key); }}><Trash2 size={14}/></button>
          ) : null}
        </div>
      ))}
      {loggedIn ? (
        <button className="scene-card scene-add" onClick={() => setCreating(true)}>
          <div className="scene-top"><span className="scene-icon"><Plus size={18}/></span><span className="scene-eyebrow">新建</span></div>
          <h3>自定义场景</h3><p>创建属于你自己的双屏场景，并在其中编排已上线应用。</p>
        </button>
      ) : null}
    </div>

    {creating ? (
      <div className="card-pad" style={{ marginTop: 16 }}>
        <div className="section-heading"><span>NEW SCENE</span><h3>新建自定义场景</h3></div>
        <div className="scene-form">
          <label>场景名称<input className="cs-input" value={form.name} maxLength={40} placeholder="如：晨间仪式" onChange={(e) => setForm({ ...form, name: e.target.value })} /></label>
          <label>副标题<input className="cs-input" value={form.eyebrow} maxLength={20} placeholder="如：我的专注空间" onChange={(e) => setForm({ ...form, eyebrow: e.target.value })} /></label>
          <label>图标<input className="cs-input" value={form.icon} maxLength={4} placeholder="自" onChange={(e) => setForm({ ...form, icon: e.target.value })} /></label>
          <label className="full">描述<input className="cs-input" value={form.desc} maxLength={120} placeholder="这个场景用来做什么" onChange={(e) => setForm({ ...form, desc: e.target.value })} /></label>
        </div>
        <div className="save-actions">
          <button className="button primary" disabled={!form.name.trim()} onClick={() => { onCreate({ name: form.name.trim(), eyebrow: form.eyebrow.trim(), desc: form.desc.trim(), icon: form.icon.trim() }).then(() => { setCreating(false); setForm({ name: "", eyebrow: "", desc: "", icon: "自" }); }); }}>创建并启用</button>
          <button className="button soft" onClick={() => setCreating(false)}>取消</button>
        </div>
      </div>
    ) : null}

    {current ? (
      <div className="card-pad" style={{ marginTop: 16 }}>
        <div className="section-heading"><span>PUBLISHED APPS · {current.name}</span><h3>为「{current.name}」场景配置已上线应用</h3></div>
        <p className="muted" style={{ fontSize: 12, lineHeight: 1.6, margin: "0 0 14px" }}>
          {showAllApps
            ? "本场景可手动编排任意已上线应用：点击下方按钮即可把应用配置到 A/B 屏，设备下次唤醒会渲染该应用的 e-ink 卡片（扫码在手机打开完整交互）。"
            : `仅显示属于「${current.name}」场景的已上线应用（应用提交时归属的 scene=${selected}）。点击即可配置到 A/B 屏。`}
        </p>
        {appErr ? <p className="form-error">{appErr}</p> :
          sceneApps.length === 0 ? (
            <div className="empty"><div><Boxes size={24} /></div><h2>本场景暂无匹配的已上线应用</h2><p>{showAllApps ? "去「已上线应用」确认有应用已审核通过并启用。" : "该预设场景当前没有归属此分类的已上线应用；如需在此放置应用，可改用「自由组合」或自定义场景（可放置任意应用）。"}</p></div>
          ) : (
            <div className="app-list">
              {sceneApps.map((a) => (
                <article key={a.app_id} className="app-card">
                  <div className="app-card-main">
                    <span className="app-badge">已上线</span><span className="app-badge isv">ISV提供</span><span className="app-badge isv">ISV提供</span>
                    <h3>{a.name}</h3>
                    <code>{a.app_id} · {a.scene}</code>
                    <p>{a.description || "暂无说明"}</p>
                  </div>
                  <div className="app-actions">
                    <button className="button sm" disabled={!hasDevice || busy === a.app_id + "a"} onClick={() => assign("a", a)}>设为 A 屏</button>
                    <button className="button sm soft" disabled={!hasDevice || busy === a.app_id + "b"} onClick={() => assign("b", a)}>设为 B 屏</button>
                  </div>
                </article>
              ))}
            </div>
          )}
      </div>
    ) : null}

    {current ? (
      <div className="selection-bar"><div><Check size={18}/><span>已选择 <b>{current.name}场景</b><small>A 屏：{current.a.title} · B 屏：{current.b.title}</small></span></div><button className="button primary" onClick={openConfig}>继续配置双屏</button></div>
    ) : null}
  </>;
}

function useDualWeatherPreview(enabled: boolean, deviceMac: string, city: string) {
  const [weatherPreview, setWeatherPreview] = useState<DualWeatherPreview | null>(null);
  const [weatherLoading, setWeatherLoading] = useState(false);
  const [weatherError, setWeatherError] = useState("");
  useEffect(() => {
    if (!enabled || !deviceMac) { setWeatherPreview(null); setWeatherError(""); setWeatherLoading(false); return; }
    let active = true;
    setWeatherLoading(true);
    setWeatherError("");
    const cityParam = city.trim() ? `?city=${encodeURIComponent(city.trim())}` : "";
    api<DualWeatherPreview>(`/api/device/${encodeURIComponent(deviceMac)}/dual-weather${cityParam}`)
      .then(data => { if (active) setWeatherPreview(data); })
      .catch(error => { if (active) { setWeatherPreview(null); setWeatherError((error as Error).message || "天气数据读取失败"); } })
      .finally(() => { if (active) setWeatherLoading(false); });
    return () => { active = false; };
  }, [enabled, deviceMac, city]);
  return { weatherPreview, weatherLoading, weatherError };
}

function PublishedApps({ onAssign, hasDevice, notify }: {
  onAssign: (s: "a" | "b", app: { app_id: string; name: string; preview_url: string }) => Promise<void>;
  hasDevice: boolean; notify: (m: string) => void;
}) {
  const [apps, setApps] = useState<{ app_id: string; name: string; description: string; scene: string; preview_url: string }[]>([]);
  const [err, setErr] = useState("");
  const [busy, setBusy] = useState("");
  useEffect(() => {
    api<{ apps: { app_id: string; name: string; description: string; scene: string; preview_url: string }[] }>("/api/published-apps")
      .then((r) => setApps(r.apps || []))
      .catch((e) => setErr((e as Error).message));
  }, []);
  return (
    <div className="card-pad">
      <div className="section-heading"><span>PUBLISHED APPS</span><h3>已上线应用</h3></div>
      <p className="muted" style={{ fontSize: 12, lineHeight: 1.6, margin: "0 0 16px" }}>
        把开发者已上线的应用配置到 A/B 屏。设备拉图时由后端生成 e-ink 卡片（应用名 + 简介 + 入口二维码），扫码即可在手机打开完整交互应用。
      </p>
      {err ? <p className="form-error">{err}</p> :
        apps.length === 0 ? (
          <div className="empty"><div><Boxes size={24} /></div><h2>暂无已上线应用</h2><p>开发者在「创作者中心」提交、管理员在「平台运营后台 → 贡献应用」审核通过后，会出现在这里。</p></div>
        ) : (
          <div className="app-list">
            {apps.map((a) => (
              <article key={a.app_id} className="app-card">
                <div className="app-card-main">
                  <span className="app-badge">已上线</span><span className="app-badge isv">ISV提供</span><span className="app-badge isv">ISV提供</span>
                  <h3>{a.name}</h3>
                  <code>{a.app_id} · {a.scene}</code>
                  <p>{a.description || "暂无说明"}</p>
                </div>
                <div className="app-actions">
                  <button className="button sm" disabled={!hasDevice || busy === a.app_id + "a"} onClick={() => { setBusy(a.app_id + "a"); onAssign("a", a).finally(() => setBusy("")); }}>设为 A 屏</button>
                  <button className="button sm soft" disabled={!hasDevice || busy === a.app_id + "b"} onClick={() => { setBusy(a.app_id + "b"); onAssign("b", a).finally(() => setBusy("")); }}>设为 B 屏</button>
                </div>
              </article>
            ))}
          </div>
        )}
    </div>
  );
}

function ScreenEditor({ scene, sceneKey, value, update, save, savedAt, content, apply, hasDevice, deviceMac, screen, setScreen }: { scene: Scene; sceneKey: SceneKey; value: { a: ScreenConfig; b: ScreenConfig }; update: (s: "a"|"b", p: Partial<ScreenConfig>) => void; save: () => void; savedAt: string; content: ContentState; apply: () => void; hasDevice: boolean; deviceMac: string; screen: { size: "4.2" | "5.83"; colors: 2 | 3 | 4 }; setScreen: (s: { size: "4.2" | "5.83"; colors: 2 | 3 | 4 }) => void }) {
  const { weatherPreview, weatherLoading, weatherError } = useDualWeatherPreview(sceneKey === "family" && value.a.mode === "family-weather", deviceMac, content.weatherCity);
  return <>
    <div className="page-intro row"><div><span className="kicker">{scene.name.toUpperCase()} SCENE</span><h2>{scene.name}场景 · 双屏配置</h2><p>A 屏负责快速文字信息，B 屏负责彩色结构与图像。</p></div><div className="save-state"><ShieldCheck size={17}/>{savedAt ? `模式保存于 ${savedAt}` : "配置尚未保存"}</div></div>
    <ScreenSettings value={screen} onChange={setScreen} />
    <div className="editor-grid">
      <ScreenForm kind="a" sceneKey={sceneKey} value={value.a} onChange={(p) => update("a", p)}/>
      <ScreenPreview kind="a" sceneKey={sceneKey} scene={scene.name} config={value.a} content={content} screen={screen} weatherPreview={weatherPreview} weatherLoading={weatherLoading} weatherError={weatherError}/>
      <ScreenForm kind="b" sceneKey={sceneKey} value={value.b} onChange={(p) => update("b", p)}/>
      <ScreenPreview kind="b" sceneKey={sceneKey} scene={scene.name} config={value.b} content={content} screen={screen}/>
    </div>
    {scene.name === "学习" ? <DualPreview content={content} screen={screen} sceneKey={sceneKey} aMode={value.a.mode || "notes"} /> : sceneKey === "family" ? <DualPreview content={content} screen={screen} sceneKey={sceneKey} aMode={value.a.mode || "family"} bMode={value.b.mode || "family"} weatherPreview={weatherPreview} weatherLoading={weatherLoading} weatherError={weatherError} /> : null}
    <div className="sticky-save"><div><b>保存双屏模式</b><span>分别写入 A/B 屏通道；旧版单屏配置不受影响。</span></div><div className="save-actions"><button className="button primary" onClick={save}><Save size={16}/>保存模式</button><button className="button soft" onClick={apply} disabled={!hasDevice} title={hasDevice ? "下发到所选设备" : "请先在设备管理绑定设备"}><Cloud size={16}/>应用到设备</button></div></div>
  </>;
}

function ScreenSettings({ value, onChange }: { value: { size: "4.2" | "5.83"; colors: 2 | 3 | 4 }; onChange: (s: { size: "4.2" | "5.83"; colors: 2 | 3 | 4 }) => void }) {
  return <div className="card-pad screen-settings">
    <div className="tt-grid-head"><span>屏幕规格</span><span className="tt-hint">需与设备实际面板一致（编译时由 menuconfig 确定）</span></div>
    <div className="ss-row">
      <label>屏幕尺寸<select className="cs-input" value={value.size} onChange={(e) => onChange({ ...value, size: e.target.value as "4.2" | "5.83" })}>
        <option value="4.2">4.2 寸（400 × 300）</option>
        <option value="5.83">5.83 寸（648 × 480）</option>
      </select></label>
      <label>B 屏色彩（FPC2）<select className="cs-input" value={String(value.colors)} onChange={(e) => onChange({ ...value, colors: parseInt(e.target.value, 10) as 2 | 3 | 4 })}>
        <option value={2}>黑白（2 色）</option>
        <option value={3}>黑白红（3 色）</option>
        <option value={4}>黑白红黄（4 色）</option>
      </select></label>
    </div>
  </div>;
}

const SCENE_MODES: Record<SceneKey, { a: { key: string; label: string }[]; b: { key: string; label: string }[] }> = {
  family: {
    a: [
      { key: "family", label: "家庭提醒（总览含日程）" },
      { key: "family-weather", label: "天气" },
      { key: "family-express-bills", label: "快递与缴费" },
      { key: "family-todos", label: "家庭待办" },
      { key: "family-shopping", label: "采购清单" },
      { key: "family-anniversary", label: "纪念日" },
      { key: "notes", label: "便签" },
      { key: "album", label: "相册" },
    ],
    b: [{ key: "family", label: "家庭共享看板（总览含日程）" }, { key: "calendar", label: "家庭日程（日历视图）" }, { key: "album", label: "相册" }],
  },
  study: {
    a: [{ key: "notes", label: "老师便签" }, { key: "homework", label: "每日作业" }, { key: "reading", label: "阅读任务" }, { key: "album", label: "相册" }],
    b: [{ key: "course", label: "课程表" }, { key: "album", label: "相册" }],
  },
  finance: {
    a: [{ key: "finance-watch", label: "自选行情" }, { key: "finance-summary", label: "市场摘要" }],
    b: [{ key: "finance-trend", label: "行情趋势" }, { key: "finance-watch", label: "自选行情" }],
  },
  health: {
    a: [{ key: "health", label: "今日健康提醒" }, { key: "health-medication", label: "用药与饮水" }],
    b: [{ key: "health-plan", label: "养生计划" }, { key: "health-sleep", label: "作息与睡眠" }],
  },
  office: {
    a: [{ key: "office", label: "工作信息" }, { key: "notes", label: "便签" }],
    b: [{ key: "album", label: "展示相册" }, { key: "office", label: "办公看板" }],
  },
  custom: {
    a: [{ key: "notes", label: "便签" }, { key: "homework", label: "待办" }, { key: "reading", label: "阅读" }, { key: "album", label: "相册" }, { key: "published-app", label: "已上线应用" }],
    b: [{ key: "course", label: "课程表" }, { key: "calendar", label: "日历" }, { key: "album", label: "相册" }, { key: "published-app", label: "已上线应用" }],
  },
};

function ScreenForm({ kind, sceneKey, value, onChange }: { kind: "a"|"b"; sceneKey: SceneKey; value: ScreenConfig; onChange: (p: Partial<ScreenConfig>) => void }) {
  const [pubApps, setPubApps] = useState<{ app_id: string; name: string; description: string; scene: string; preview_url: string }[]>([]);
  const [pubErr, setPubErr] = useState("");
  useEffect(() => {
    if (value.mode !== "published-app") return;
    api<{ apps: { app_id: string; name: string; description: string; scene: string; preview_url: string }[] }>("/api/published-apps")
      .then((r) => setPubApps(r.apps || []))
      .catch((e) => setPubErr((e as Error).message));
  }, [value.mode]);
  const baseModes = (SCENE_MODES[sceneKey] || SCENE_MODES.custom)[kind] || [];
  const modes = value.mode === "published-app" && !baseModes.some((m) => m.key === "published-app")
    ? [...baseModes, { key: "published-app", label: "已上线应用" }]
    : baseModes;
  const familySource = (mode: string) => kind === "b"
    ? mode === "album" ? "后台相册（内容管理）" : "后台家庭日程（内容管理）"
    : ({ family: "家庭内容管理（天气、快递缴费、待办、日程、采购、纪念日）", "family-weather": "和风天气 API（设备城市）", "family-express-bills": "家庭内容管理（快递与缴费）", "family-todos": "家庭内容管理（待办与便签）", "family-shopping": "家庭内容管理（采购清单）", "family-anniversary": "家庭内容管理（纪念日）", notes: "后台便签", album: "后台相册（内容管理）" } as Record<string, string>)[mode] || "家庭内容管理";
  const sourceOptions = [
    ...(kind === "b" ? ["课程表", "天气", "每日作业"] : []),
    "后台手动配置", "相册（后台上传）", "飞书日历", "TF 卡", "行情数据源",
  ];
  const refreshOptions = ["每 5 分钟", "每 10 分钟", "每 15 分钟", "每 30 分钟", "每 12 小时", "每 24 小时"];
  return <div className={`screen-form ${kind}`}>
    <div className="screen-form-head"><div><span>{kind.toUpperCase()} 屏</span><h3>{kind === "a" ? "FPC1 · 黑白快刷" : "FPC2 · 三色 / 四色"}</h3></div><span className="screen-status"><i/>已启用</span></div>
    <label>屏幕内容名称<input value={value.title} onChange={(e) => onChange({ title: e.target.value })}/></label>
    <label>{kind.toUpperCase()} 屏模式<select value={value.mode || modes[0].key} onChange={(e) => onChange({ mode: e.target.value, ...(sceneKey === "family" ? { source: familySource(e.target.value) } : {}) })}>{modes.map((m) => <option key={m.key} value={m.key}>{m.label}</option>)}</select></label>
    {value.mode === "published-app" ? (
      <div className="pub-app-pick">
        <label>ISV 应用
          {pubApps.length === 0 ? (
            <span className="pub-app-empty">暂无已上线应用，请先在「已上线应用」确认开发者提交的应用已审核通过并启用。</span>
          ) : (
            <select value={value.appId || ""} onChange={(e) => { const a = pubApps.find((x) => x.app_id === e.target.value); if (a) onChange({ mode: "published-app", appId: a.app_id, title: a.name, source: a.preview_url, refresh: "每次唤醒" }); }}>
              <option value="">请选择要展示的 ISV 应用…</option>
              {pubApps.map((a) => <option key={a.app_id} value={a.app_id}>{a.name}（ISV提供）</option>)}
            </select>
          )}
        </label>
        {pubErr ? <p className="form-error">{pubErr}</p> : null}
        {value.appId ? (
          <p className="tt-hint">当前展示：<b>{value.title}</b> · ISV提供。设备拉图时由后端生成该应用的 e-ink 卡片（应用名 + 简介 + 入口二维码），扫码在手机打开完整交互。</p>
        ) : (
          <p className="tt-hint">选择「已上线应用」模式后，请从上方选择一款已审核上线的 ISV 应用，保存即下发到本屏。</p>
        )}
      </div>
    ) : null}
    {sceneKey === "family" && <p className="tt-hint">家庭提醒由所选模式决定读取的内容，来源标签自动对应「内容管理」中的相应数据，不能手动选成其他来源；家庭日程在「内容管理 → 家庭日程」维护。{value.mode === "family-weather" ? "城市在 dual「个人中心 → 家庭与设备位置」设置；此处会请求实时天气，设备刷新时也使用同一城市。" : ""}</p>}
    <div className="field-row"><label>数据来源{sceneKey === "family" ? <input value={familySource(value.mode || modes[0].key)} readOnly /> : <select value={value.source} onChange={(e) => onChange({ source: e.target.value })}>{[value.source, ...sourceOptions.filter((source) => source !== value.source)].map((source) => <option key={source} value={source}>{source}</option>)}</select>}</label><label>刷新频率<select value={value.refresh} onChange={(e) => onChange({ refresh: e.target.value })}>{[value.refresh, ...refreshOptions.filter((refresh) => refresh !== value.refresh)].map((refresh) => <option key={refresh} value={refresh}>{refresh}</option>)}</select></label></div>
    <div className="module-label"><span>内容模块</span><button onClick={() => { const preset = ["相册", "天气", "倒计时"].find((m) => !value.modules.includes(m)) || "新模块"; onChange({ modules: [...value.modules, preset] }); }}><Plus size={14}/>添加</button></div>
    <div className="modules">{value.modules.map((module, index) => <div key={`${module}-${index}`}><span className="drag">⋮⋮</span><input value={module} onChange={(e) => { const next = [...value.modules]; next[index] = e.target.value; onChange({ modules: next }); }}/><button aria-label="删除模块" onClick={() => onChange({ modules: value.modules.filter((_, i) => i !== index) })}><X size={14}/></button></div>)}</div>
  </div>;
}

function ScreenPreview({ kind, sceneKey, scene, config, content, screen, weatherPreview, weatherLoading, weatherError }: { kind: "a"|"b"; sceneKey: SceneKey; scene: string; config: ScreenConfig; content: ContentState; screen: { size: "4.2" | "5.83"; colors: 2 | 3 | 4 }; weatherPreview?: DualWeatherPreview | null; weatherLoading?: boolean; weatherError?: string }) {
  const dims = screen.size === "5.83" ? "648 × 480" : "400 × 300";
  let body: React.ReactNode;
  if (sceneKey === "study" && kind === "a") {
    body = aModePreview(config.mode || "notes", content);
  } else if (sceneKey === "study" && kind === "b") {
    // 与后端 render_dual_screen 一致：今日无课且相册有照片 → 相册兜底
    const todayIdx = (new Date().getDay() + 6) % 7; // 周一=0
    const hasCourse = (content.courseTable.periods || []).some((_, p) => !!content.courseTable.grid?.[todayIdx]?.[p]?.name);
    body = !hasCourse && (content.album?.photos?.length || 0) > 0
      ? <AlbumPreview album={content.album} />
      : <CoursePreview ct={content.courseTable} colors={screen.colors} />;
  } else if (sceneKey === "health") {
    body = <SceneModePreview title={config.title} modules={config.modules} note="提醒内容可在内容管理中维护" />;
  } else if (sceneKey === "finance") {
    body = <SceneModePreview title={config.title} modules={config.modules} note="当前尚未连接实时行情源" />;
  } else if (sceneKey === "family" && kind === "a") {
    const modeLabels: Record<string, string> = {
      family: "家庭提醒（总览含日程）", "family-weather": "天气", "family-express-bills": "快递与缴费",
      "family-todos": "家庭待办", "family-shopping": "采购清单", "family-anniversary": "纪念日",
    };
    const modeNotes: Record<string, string> = {
      family: `家庭内容总览 · 日程 ${content.familySchedule.length} 项 · 采购 ${content.familyShopping.filter(x => !x.done).length} 项`, "family-weather": weatherLoading ? "正在读取实时天气数据…" : weatherError ? `天气数据读取失败 · ${weatherError}` : "实时天气与未来预报",
      "family-express-bills": `${content.express.filter((x) => x.status !== "signed").length} 个未签收快递 · ${content.bills.filter((x) => !x.paid).length} 项待缴费`,
      "family-todos": `待办与便签 · ${content.homework.filter((x) => !x.done).length} 项待办`,
      "family-shopping": `${content.familyShopping.filter(x => !x.done).length} 项未完成采购`, "family-anniversary": `${content.familyAnniversaries.length} 个家庭纪念日`,
    };
    const selectedMode = config.mode || "family";
    const eventRows = content.familySchedule.slice(0, 3).map(x => `${x.date}${x.time ? ` ${x.time}` : ""} ${x.title}`);
    const previewModules = selectedMode === "family"
      ? [...eventRows, ...config.modules]
      : selectedMode === "family-shopping" ? content.familyShopping.filter(x => !x.done).slice(0, 5).map(x => x.text)
        : selectedMode === "family-anniversary" ? content.familyAnniversaries.slice(0, 5).map(x => `${x.date} ${x.title}`)
          : selectedMode === "family-todos" ? content.homework.filter(x => !x.done).slice(0, 5).map(x => x.text)
            : [modeLabels[selectedMode] || config.title];
    body = selectedMode === "family-weather"
      ? <WeatherBoard data={weatherPreview} loading={!!weatherLoading} error={weatherError || ""} city={content.weatherCity || "设备城市"} />
      : selectedMode === "family" ? <FamilyTodayScreen content={content} />
      : <SceneModePreview title={modeLabels[selectedMode] || config.title} modules={previewModules} note={modeNotes[selectedMode] || "家庭场景"} />;
  } else if (sceneKey === "family" && kind === "b") {
    const isAlbum = config.mode === "album";
    body = isAlbum ? <AlbumPreview album={content.album} /> : <FamilyWeekScreen content={content} />;
  } else if (sceneKey === "office") {
    body = <SceneModePreview title={config.title} modules={config.modules} note="办公场景" />;
  } else {
    body = <SceneModePreview title={config.title} modules={config.modules} note="自定义场景" />;
  }
  const colorBadge = kind === "b" ? (screen.colors === 2 ? "黑白" : screen.colors === 3 ? "黑白红" : "黑白红黄") : null;
  return <div className={`preview-card ${kind}`}><div className="preview-meta"><span>实时预览</span><small>{dims}{colorBadge ? ` · ${colorBadge}` : ""}</small></div>{body}</div>;
}

function CalendarPreview() { return <div className="calendar-preview"><div className="week">{["一","二","三","四","五","六","日"].map(v=><b key={v}>{v}</b>)}</div><div className="days">{Array.from({length:28},(_,i)=><span key={i} className={i===15?"today":i===17?"event":""}>{i+1}</span>)}</div><p><i/>今天 · 家庭日晚餐</p></div>; }
function WeatherBoard({ data, loading, error, city }: { data?: DualWeatherPreview | null; loading: boolean; error: string; city: string }) {
  if (!data) return <div className="eink-screen weather-board"><div className="weather-board-head"><b>{city}</b><span>{loading ? "正在获取天气…" : "实时天气"}</span></div><div className="weather-board-empty">{error || (loading ? "正在读取天气预报" : "选择设备后加载实时天气")}</div></div>;
  const currentTemp = data.current_temp || data.today_temp || "--";
  return <div className="eink-screen weather-board">
    <div className="weather-board-head"><b>{data.city || city}</b><span>实时天气</span></div>
    <div className="weather-board-main">
      <div className="weather-current"><strong>{currentTemp}°C</strong><div className="weather-current-desc"><WeatherIcon code={data.today_code} description={data.today_desc}/><span>{data.today_desc}</span></div><small>{data.today_low}°C / {data.today_high}°C</small></div>
      <div className="weather-forecast">{(data.forecast || []).slice(0, 4).map((day, i) => <div className="weather-day" key={`${day.date}-${i}`}><b>{day.day}</b><small>{day.date}</small><WeatherIcon code={day.code} description={day.desc}/><span>{day.desc}</span><small>{day.temp_range.replaceAll("℃", "°")}</small></div>)}</div>
    </div>
    <div className="weather-board-meta"><span>湿度 {data.today_humidity || "--"}%</span><span>{data.today_wind_dir || ""} {data.today_wind_level || ""}</span></div>
    <div className="weather-board-sun">日出 {data.sunrise || "--:--"}<i/>日落 {data.sunset || "--:--"}</div>
    <div className="weather-board-advice">{data.advice || "注意根据天气添减衣物"}</div>
    <div className="weather-board-footer"><span><Cloud size={13}/> 天气</span><span>实时预报</span></div>
  </div>;
}
function WeatherIcon({ code, description }: { code?: number; description: string }) { const d = description || ""; const props = { size: 25, strokeWidth: 1.8 }; if (/雨|雷|阵雨/.test(d) || (code !== undefined && [51,53,55,61,63,65,80,81,82,95,96,99].includes(code))) return <CloudRain {...props}/>; if (/雪/.test(d) || (code !== undefined && [71,73,75,77,85,86].includes(code))) return <CloudSnow {...props}/>; if (/晴/.test(d) || code === 0) return <Sun {...props}/>; if (/多云/.test(d) || code === 1 || code === 2) return <CloudSun {...props}/>; return <Cloud {...props}/>; }
function SceneModePreview({ title, modules, note }: { title: string; modules: string[]; note: string }) { return <div className="eink-screen"><div className="eink-title"><b>{title}</b><span>场景专属</span></div><div className="eink-list">{modules.slice(0, 5).map((m, i) => <div key={`${m}-${i}`}><span>{i === 0 ? "●" : "○"} {m}</span><b>{i === 0 ? "今日" : ""}</b></div>)}</div><small className="eink-sub">{note}</small></div>; }
function BarsPreview({ photo }: { photo?: boolean }) { return photo ? <div className="photo-preview"><span>照片 / 海报预览</span><div/><p>来自后台相册 · 3 张</p></div> : <div className="bars-preview"><div>{[28,42,35,58,51,67,62,79,70,86].map((v,i)=><i key={i} style={{height:`${v}%`}}/>)}</div><p><span>黄金 AU</span><b>▲ 0.84%</b></p><p><span>沪深 300</span><b>▼ 0.26%</b></p></div>; }

function ScreenSummary({ kind, config }: { kind: "a"|"b"; config: ScreenConfig }) { return <div className={`screen-summary ${kind}`}><div className="summary-head"><span>{kind.toUpperCase()} 屏</span><div><b>{config.title}</b><small>{kind === "a" ? "FPC1 · 黑白快刷" : "FPC2 · 三/四色"}</small></div><i>已启用</i></div><div className="summary-body">{config.modules.slice(0,4).map(m=><span key={m}><Check size={13}/>{m}</span>)}</div><div className="summary-foot"><span><RefreshCw size={13}/>{config.refresh}</span><span><Cloud size={13}/>{config.source}</span></div></div>; }

function DeviceManager({ devices, user, refresh, openAuth, openPair, notify, choose }: { devices: Device[]; user: User|null; refresh:()=>Promise<void>; openAuth:()=>void; openPair:()=>void; notify:(m:string)=>void; choose:(m:string)=>void }) {
  async function unbind(device: Device) { if (!confirm(`确认解绑 ${device.nickname || device.mac}？`)) return; try { await api(`/api/user/devices/${encodeURIComponent(device.mac)}`, { method: "DELETE" }); await refresh(); notify("设备已解绑"); } catch (e) { notify((e as Error).message); } }
  if (!user) return <EmptyState icon={<ShieldCheck/>} title="登录后管理你的设备" text="这里复用 小悟AI 的账号、设备绑定和所有权体系。" action="登录账号" onAction={openAuth}/>;
  return <><div className="page-intro row"><div><span className="kicker">DEVICE FLEET</span><h2>设备与绑定</h2><p>绑定关系与原 小悟AI 后台共享，不会创建重复设备。</p></div><button className="button primary" onClick={openPair}><Plus size={16}/>绑定设备</button></div>
    {devices.length === 0 ? <EmptyState icon={<Cpu/>} title="还没有绑定设备" text="输入设备屏幕上的配对码，或使用设备 MAC 地址绑定。" action="绑定第一台设备" onAction={openPair}/> : <div className="device-table"><div className="table-head"><span>设备</span><span>连接状态</span><span>角色</span><span>最后活动</span><span/></div>{devices.map(d=><div className="device-row" key={d.mac}><div className="device-name"><i><Cpu size={19}/></i><span><b>{d.nickname || "双屏墨水屏"}</b><small>{d.mac}</small></span></div><span className={`status-pill ${d.is_online?"on":""}`}><i/>{d.is_online?"在线":"离线"}</span><span>{d.role === "owner" ? "所有者" : "共享成员"}</span><span>{d.last_seen || "暂无记录"}</span><div className="row-actions"><button onClick={()=>choose(d.mac)}>配置</button><button className="danger" aria-label="解绑" onClick={()=>void unbind(d)}><Trash2 size={15}/></button></div></div>)}</div>}
  </>;
}

function MembersPanel({ device, user, notify }: { device?: Device; user: User|null; notify:(m:string)=>void }) {
  const [members, setMembers] = useState<Array<{user_id:number;username:string;role?:string}>>([]); const [name,setName]=useState(""); const [busy,setBusy]=useState(false);
  useEffect(()=>{ if (!device||!user) return; api<{members?:typeof members}>(`/api/user/devices/${encodeURIComponent(device.mac)}/members`).then(r=>setMembers(r.members||[])).catch(()=>setMembers([])); },[device,user]);
  async function share(){ if(!device||!name.trim())return;setBusy(true);try{await api(`/api/user/devices/${encodeURIComponent(device.mac)}/share`,{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({username:name.trim()})});setName("");notify("共享邀请已发送");}catch(e){notify((e as Error).message)}finally{setBusy(false)}}
  if(!user)return <EmptyState icon={<Users/>} title="登录后查看共享成员" text="家庭、学习场景可以与其他 小悟AI 账号协作。" action="请先登录" onAction={()=>notify("请从右上角登录")}/>;
  if(!device)return <EmptyState icon={<Cpu/>} title="请先绑定设备" text="绑定设备后即可邀请家庭成员或同事共同维护。" action="前往设备管理" onAction={()=>notify("请在设备管理中绑定设备")}/>;
  return <><div className="page-intro"><span className="kicker">COLLABORATION</span><h2>{device.nickname||"双屏设备"} · 共享成员</h2><p>成员体系复用 小悟AI。所有者可以邀请、审批和移除成员。</p></div><div className="member-layout"><div className="member-card"><h3>邀请成员</h3><p>输入对方的 小悟AI 用户名。</p><div className="inline-form"><input placeholder="用户名" value={name} onChange={e=>setName(e.target.value)}/><button className="button primary" disabled={busy} onClick={()=>void share()}>{busy?<LoaderCircle className="spin" size={16}/>:<Plus size={16}/>}发送邀请</button></div></div><div className="member-card"><h3>当前成员 <span>{members.length}</span></h3>{members.length?members.map(m=><div className="member" key={m.user_id}><CircleUserRound/><span><b>{m.username}</b><small>{m.role==="owner"?"所有者":"共享成员"}</small></span></div>):<p className="muted">暂未读取到共享成员。</p>}</div></div></>;
}

type TeacherAuth = { id:number;auth_code:string;auth_name:string;permission:string;expire_time:string|null;is_frozen:boolean;created_at?:string;expired?:boolean };
function TeacherAuthPanel({ device, user, notify }: { device?: Device; user: User|null; notify:(m:string)=>void }) {
  const [items, setItems] = useState<TeacherAuth[]>([]);
  const [name, setName] = useState("");
  const [days, setDays] = useState<string>("30");
  const [perm, setPerm] = useState<"only_note"|"note_and_homework">("only_note");
  const [busy, setBusy] = useState(false);
  const [generated, setGenerated] = useState<{auth_code:string;expire_time:string|null;qr_text:string}|null>(null);

  async function refresh() {
    if (!device) return;
    try {
      const r = await api<{items: TeacherAuth[]}>(`/api/teacher-auth/list?mac=${encodeURIComponent(device.mac)}`);
      setItems(r.items || []);
    } catch (e) { notify((e as Error).message); }
  }
  useEffect(() => { if (device && user) { void refresh(); } else { setItems([]); } }, [device, user]);

  async function generate() {
    if (!device) return;
    setBusy(true);
    try {
      const r = await api<{auth_code:string;expire_time:string|null;qr_text:string}>("/api/teacher-auth/generate", {
        method: "POST", headers: {"Content-Type":"application/json"},
        body: JSON.stringify({ device_mac: device.mac, auth_name: name.trim() || "老师", validity_days: Number(days), permission: perm }),
      });
      setGenerated({ auth_code: r.auth_code, expire_time: r.expire_time, qr_text: r.qr_text });
      setName("");
      notify("授权码已生成，请发给老师");
      await refresh();
    } catch (e) { notify((e as Error).message); } finally { setBusy(false); }
  }

  async function toggleFreeze(it: TeacherAuth) {
    if (!device) return;
    try {
      await api("/api/teacher-auth/freeze", { method:"POST", headers:{"Content-Type":"application/json"},
        body: JSON.stringify({ device_mac: device.mac, auth_code: it.auth_code, frozen: it.is_frozen ? 0 : 1 }) });
      notify(it.is_frozen ? "已解冻" : "已冻结");
      await refresh();
    } catch (e) { notify((e as Error).message); }
  }

  if (!user) return <EmptyState icon={<KeyRound/>} title="登录后管理老师授权" text="家长登录后才能为设备生成老师授权码。" action="请先登录" onAction={()=>notify("请从右上角登录")}/>;
  if (!device) return <EmptyState icon={<Cpu/>} title="请先绑定设备" text="绑定设备后即可为老师生成推送授权码。" action="前往设备管理" onAction={()=>notify("请在设备管理中绑定设备")}/>;

  return <>
    <div className="page-intro"><span className="kicker">TEACHER ACCESS</span><h2>{device.nickname||"双屏设备"} · 老师授权</h2><p>老师免注册：生成 6 位授权码发给老师，老师凭码在「微信小程序 / 扫码」推送便签或作业。授权码可随时冻结或设有效期，过期自动失效。</p></div>
    <div className="member-layout">
      <div className="member-card">
        <h3>生成授权码</h3>
        <p>填写老师称呼、有效期与权限。</p>
        {generated ? <div className="auth-code-box"><span className="kicker">NEW CODE</span><b>{generated.auth_code}</b><small>{generated.expire_time ? `有效期至 ${generated.expire_time}` : "长期有效"}</small><code>{generated.qr_text}</code><button className="button soft" onClick={()=>{ navigator.clipboard?.writeText(generated.auth_code); notify("已复制授权码"); }}>复制授权码</button></div> : null}
        <div className="inline-form col">
          <input className="cs-input" placeholder="老师称呼（默认：老师）" value={name} onChange={e=>setName(e.target.value)}/>
          <div className="row">
            <select className="cs-input" value={days} onChange={e=>setDays(e.target.value)}>
              <option value="7">7 天</option>
              <option value="30">30 天</option>
              <option value="0">长期</option>
            </select>
            <select className="cs-input" value={perm} onChange={e=>setPerm(e.target.value as "only_note"|"note_and_homework")}>
              <option value="only_note">仅便签</option>
              <option value="note_and_homework">便签 + 作业</option>
            </select>
          </div>
          <button className="button primary" disabled={busy} onClick={()=>void generate()}>{busy?<LoaderCircle className="spin" size={16}/>:<Plus size={16}/>}生成授权码</button>
        </div>
      </div>
      <div className="member-card">
        <h3>授权码列表 <span>{items.length}</span></h3>
        {items.length ? items.map(it => (
          <div className={`member auth-row${it.is_frozen?" frozen":""}${it.expired?" expired":""}`} key={it.id}>
            <KeyRound/>
            <span><b>{it.auth_name}</b><small>{it.permission==="note_and_homework"?"便签+作业":"仅便签"} · {it.auth_code}{it.expire_time?` · ${it.expire_time}`:""}</small></span>
            <span className="auth-tag">{it.is_frozen?"已冻结":it.expired?"已过期":"有效"}</span>
            <button className="button soft sm" onClick={()=>void toggleFreeze(it)}>{it.is_frozen?"解冻":"冻结"}</button>
          </div>
        )) : <p className="muted">还没有生成授权码。</p>}
      </div>
    </div>
  </>;
}

type LLMConfig = {
  llm_access_mode?: string; provider?: string; model?: string; base_url?: string;
  image_provider?: string; image_model?: string; image_base_url?: string;
  llm_api_key_set?: boolean; image_api_key_set?: boolean;
};
type ChildProfile = {
  name?: string; birth?: string; gender?: string; school?: string; grade_override?: string;
  grade?: { grade_text?: string; stage?: string; age?: number; note?: string };
};

// 依据出生日期推算中国学制年级（9 月 1 日为入学年龄 cutoff，6 周岁入小学一年级）
function computeChildStage(birth: string): { age: number; grade_text: string; stage: string } {
  if (!birth) return { age: 0, grade_text: "—", stage: "—" };
  const b = new Date(birth + "T00:00:00");
  if (isNaN(b.getTime())) return { age: 0, grade_text: "日期无效", stage: "—" };
  const now = new Date();
  let age = now.getFullYear() - b.getFullYear();
  if (now.getMonth() < b.getMonth() || (now.getMonth() === b.getMonth() && now.getDate() < b.getDate())) age--;
  const schoolYearStart = now.getMonth() >= 8 ? now.getFullYear() : now.getFullYear() - 1;
  const bornAfterAug31 = b.getMonth() > 7;
  const entryYear1 = b.getFullYear() + 6 + (bornAfterAug31 ? 1 : 0);
  let g = (schoolYearStart - entryYear1) + 1;
  if (g <= 0) return { age, grade_text: age >= 3 ? "幼儿园" : "未到入园年龄", stage: age >= 3 ? "幼儿园" : "学龄前" };
  if (g <= 6) return { age, grade_text: `小学 ${g} 年级`, stage: "小学" };
  if (g <= 9) return { age, grade_text: `初中 ${g - 6} 年级`, stage: "初中" };
  if (g <= 12) return { age, grade_text: `高中 ${g - 9} 年级`, stage: "高中" };
  return { age, grade_text: "已高中毕业", stage: "毕业" };
}

function ProfilePanel({ user, notify, content, setContent, save, apply, hasDevice }: { user: User | null; notify: (m: string) => void; content: ContentState; setContent: React.Dispatch<React.SetStateAction<ContentState>>; save: () => void; apply: () => void; hasDevice: boolean }) {
  const [llm, setLlm] = useState<LLMConfig>({});
  const [apiKey, setApiKey] = useState("");
  const [imgApiKey, setImgApiKey] = useState("");
  const [llmBusy, setLlmBusy] = useState(false);
  const [child, setChild] = useState<ChildProfile>({});
  const [childBusy, setChildBusy] = useState(false);

  async function load() {
    if (!user) return;
    try {
      const p = await api<{ llm_config?: LLMConfig }>("/api/user/profile");
      setLlm(p.llm_config || {});
    } catch (e) { notify((e as Error).message); }
    try {
      const c = await api<{ child: ChildProfile | null }>("/api/user/child");
      setChild(c.child || {});
    } catch (e) { notify((e as Error).message); }
  }
  useEffect(() => { if (user) { void load(); } else { setLlm({}); setChild({}); } }, [user]);

  const derived = computeChildStage(child.birth || "");

  async function saveLlm() {
    setLlmBusy(true);
    try {
      const body: Record<string, unknown> = {
        llm_access_mode: llm.llm_access_mode || "preset",
        provider: llm.provider || "deepseek",
        model: llm.model || "",
        base_url: llm.base_url || "",
        image_provider: llm.image_provider || "aliyun",
        image_model: llm.image_model || "qwen-vl-max",
        image_base_url: llm.image_base_url || "",
      };
      if (apiKey) body.api_key = apiKey;            // 空 = 保留已有 Key
      if (imgApiKey) body.image_api_key = imgApiKey;
      await api("/api/user/profile/llm", { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
      setApiKey(""); setImgApiKey("");
      notify("大模型配置已保存");
      await load();
    } catch (e) { notify((e as Error).message); } finally { setLlmBusy(false); }
  }

  async function saveChild() {
    setChildBusy(true);
    try {
      await api("/api/user/child", {
        method: "PUT", headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          name: child.name || "", birth: child.birth || "", gender: child.gender || "",
          school: child.school || "", grade_override: child.grade_override || "",
        }),
      });
      notify("孩子信息已保存");
    } catch (e) { notify((e as Error).message); } finally { setChildBusy(false); }
  }

  if (!user) return <EmptyState icon={<UserCog />} title="登录后查看个人信息" text="在这里配置大模型 Key 与孩子资料，供作业图片识别（OCR）等功能使用。" action="请先登录" onAction={() => notify("请从右上角登录")} />;

  return <>
    <div className="page-intro"><span className="kicker">PROFILE</span><h2>个人中心</h2><p>管理账号与孩子资料，也维护家庭成员和设备所在城市，供家庭共享日程、天气看板使用。</p></div>
    <div className="profile-grid">
      <div className="member-card">
        <h3>大模型配置</h3>
        <label className="mini">接入方式</label>
        <select className="cs-input" value={llm.llm_access_mode || "preset"} onChange={(e) => setLlm({ ...llm, llm_access_mode: e.target.value })}>
          <option value="platform">平台额度（免费，无需 Key）</option>
          <option value="preset">自带 Key（预设厂商）</option>
          <option value="custom_openai">自定义 OpenAI 兼容端点</option>
        </select>
        {llm.llm_access_mode !== "platform" ? <>
          <label className="mini">文本模型厂商</label>
          <select className="cs-input" value={llm.provider || "deepseek"} onChange={(e) => setLlm({ ...llm, provider: e.target.value })}>
            <option value="deepseek">DeepSeek</option>
            <option value="aliyun">阿里云通义</option>
            <option value="moonshot">Kimi（月之暗面）</option>
            <option value="siliconflow">硅基流动 SiliconFlow</option>
            <option value="openai">OpenAI</option>
          </select>
          <label className="mini">文本模型</label>
          <input className="cs-input" placeholder={llm.provider === "siliconflow" ? "如 deepseek-ai/DeepSeek-V3.2" : "如 deepseek-chat"} value={llm.model || ""} onChange={(e) => setLlm({ ...llm, model: e.target.value })} />
          {llm.llm_access_mode === "custom_openai" ? <>
            <label className="mini">自定义端点 base_url</label>
            <input className="cs-input" placeholder="https://..." value={llm.base_url || ""} onChange={(e) => setLlm({ ...llm, base_url: e.target.value })} />
          </> : null}
          <label className="mini">API Key {llm.llm_api_key_set ? "（已保存，留空保持不变）" : ""}</label>
          <input className="cs-input" type="password" placeholder={llm.llm_api_key_set ? "已保存，重新输入可覆盖" : "sk-..."} value={apiKey} onChange={(e) => setApiKey(e.target.value)} />
        </> : null}
        <label className="mini">视觉模型厂商（作业图片识别 OCR 使用）</label>
        <div className="row">
          <select className="cs-input" value={llm.image_provider || "aliyun"} onChange={(e) => setLlm({ ...llm, image_provider: e.target.value })}>
            <option value="aliyun">阿里云通义</option>
            <option value="siliconflow">硅基流动 SiliconFlow</option>
            <option value="openai">OpenAI</option>
          </select>
          <input className="cs-input" placeholder={llm.image_provider === "siliconflow" ? "如 Qwen/Qwen2.5-VL-32B-Instruct" : "如 qwen-vl-max"} value={llm.image_model || ""} onChange={(e) => setLlm({ ...llm, image_model: e.target.value })} />
        </div>
        {llm.llm_access_mode !== "platform" ? <>
          <label className="mini">视觉 API Key {llm.image_api_key_set ? "（已保存，留空保持不变）" : ""}</label>
          <input className="cs-input" type="password" placeholder={llm.image_api_key_set ? "已保存，重新输入可覆盖" : "sk-..."} value={imgApiKey} onChange={(e) => setImgApiKey(e.target.value)} />
        </> : null}
        <button className="button primary" disabled={llmBusy} onClick={() => void saveLlm()}>{llmBusy ? <LoaderCircle className="spin" size={16} /> : <Save size={16} />}保存大模型配置</button>
      </div>

      <div className="member-card">
        <h3>孩子信息</h3>
        <label className="mini">姓名</label>
        <input className="cs-input" placeholder="孩子昵称 / 姓名" value={child.name || ""} onChange={(e) => setChild({ ...child, name: e.target.value })} />
        <label className="mini">出生日期</label>
        <input className="cs-input" type="date" value={child.birth || ""} onChange={(e) => setChild({ ...child, birth: e.target.value })} />
        <label className="mini">性别</label>
        <select className="cs-input" value={child.gender || ""} onChange={(e) => setChild({ ...child, gender: e.target.value })}>
          <option value="">不填</option>
          <option value="男">男</option>
          <option value="女">女</option>
        </select>
        <label className="mini">学校（选填）</label>
        <input className="cs-input" placeholder="如 某某实验小学" value={child.school || ""} onChange={(e) => setChild({ ...child, school: e.target.value })} />
        <label className="mini">年级覆盖（选填，留空则按出生日期自动推算）</label>
        <input className="cs-input" placeholder="如 小学三年级" value={child.grade_override || ""} onChange={(e) => setChild({ ...child, grade_override: e.target.value })} />
        <div className="grade-badge">
          <span className="kicker">当前推算</span>
          <b>{child.grade_override ? child.grade_override : derived.grade_text}</b>
          <small>{derived.age ? `${derived.age} 岁 · ${derived.stage}` : (child.birth ? "" : "填写出生日期后自动推算")}</small>
        </div>
        <button className="button primary" disabled={childBusy} onClick={() => void saveChild()}>{childBusy ? <LoaderCircle className="spin" size={16} /> : <Save size={16} />}保存孩子信息</button>
      </div>
    </div>
    <FamilyHomeSettings content={content} setContent={setContent} save={save} apply={apply} hasDevice={hasDevice} />
  </>;
}

function FamilyHomeSettings({ content, setContent, save, apply, hasDevice }: { content: ContentState; setContent: React.Dispatch<React.SetStateAction<ContentState>>; save: () => void; apply: () => void; hasDevice: boolean }) {
  const [newMember, setNewMember] = useState("");
  const [newMemberAdmin, setNewMemberAdmin] = useState(false);
  function addMember() {
    const name = newMember.trim();
    if (!name || content.familyMembers.some(member => member.name === name)) return;
    const makeAdmin = newMemberAdmin || content.familyMembers.length === 0;
    setContent({ ...content, familyMembers: [...content.familyMembers.map(member => makeAdmin ? { ...member, role: "member" as const } : member), { name, role: makeAdmin ? "admin" : "member" }] });
    setNewMember("");
    setNewMemberAdmin(false);
  }
  function setAdmin(name: string) {
    setContent({ ...content, familyMembers: content.familyMembers.map(member => ({ ...member, role: member.name === name ? "admin" : "member" })) });
  }
  return <section className="member-card family-home-settings">
    <h3>家庭与设备位置</h3>
    <p className="tt-hint">标记家庭日程中的设备管理员。此角色用于家庭资料展示；设备绑定、解绑和账号权限仍由当前登录账号的设备所有者权限控制。</p>
    <label className="mini">设备所在城市</label>
    <input className="cs-input" placeholder="例如：杭州市" value={content.weatherCity} onChange={e => setContent({ ...content, weatherCity: e.target.value })} />
    <p className="tt-hint">家庭天气优先使用这里的城市；留空时才回退到 小悟AI 的天气模式/全局位置。</p>
    <label className="mini">家庭成员</label>
    <div className="inline-form"><input placeholder="添加家庭成员姓名/称呼" value={newMember} onChange={e => setNewMember(e.target.value)} onKeyDown={e => { if (e.key === "Enter") addMember(); }} /><button className="button" onClick={addMember}><Plus size={15} />添加成员</button></div>
    <label className="family-admin-option"><input type="checkbox" checked={newMemberAdmin || content.familyMembers.length === 0} disabled={content.familyMembers.length === 0} onChange={e => setNewMemberAdmin(e.target.checked)} />设为设备管理员{content.familyMembers.length === 0 ? "（首位成员自动设为管理员）" : ""}</label>
    <div className="family-member-chips">{content.familyMembers.length ? content.familyMembers.map(member => <span className="family-member-chip" key={member.name}><b>{member.name}</b>{member.role === "admin" ? <em>设备管理员</em> : <button type="button" onClick={() => setAdmin(member.name)}>设为管理员</button>}<button aria-label={`移除${member.name}`} onClick={() => setContent({ ...content, familyMembers: content.familyMembers.filter(x => x.name !== member.name) })}><X size={13} /></button></span>) : <small className="tt-empty">还没有家庭成员，添加后可在日程中选择。</small>}</div>
    <div className="save-actions"><button className="button primary" onClick={save}><Save size={15} />保存家庭设置</button><button className="button soft" onClick={apply} disabled={!hasDevice}><Cloud size={15} />应用到设备</button></div>
  </section>;
}

type ServiceItem = {
  key: string;
  label: string;
  secret: boolean;
  hint: string;
  placeholder: string;
  masked: string;
  configured: boolean;
  source: "db" | "env" | "none";
};

function ServiceConfigPanel({ user, notify }: { user: User | null; notify: (m: string) => void }) {
  const [items, setItems] = useState<ServiceItem[]>([]);
  const [drafts, setDrafts] = useState<Record<string, string>>({});
  const [reveal, setReveal] = useState<Record<string, boolean>>({});
  const [busy, setBusy] = useState(false);
  const [expressConfigured, setExpressConfigured] = useState<boolean | null>(null);

  async function load() {
    if (!user) return;
    try {
      const r = await api<{ items: ServiceItem[]; express_configured: boolean }>("/api/settings/service");
      setItems(r.items || []);
      setExpressConfigured(r.express_configured);
    } catch (e) { notify((e as Error).message); }
  }
  useEffect(() => { if (user) { void load(); } else { setItems([]); } }, [user]);

  function setDraft(key: string, v: string) {
    setDrafts((prev) => ({ ...prev, [key]: v }));
  }

  async function save() {
    setBusy(true);
    try {
      const changed = items.filter((it) => (drafts[it.key] ?? "") !== "");
      if (changed.length === 0) { notify("没有要保存的变更（留空的字段保持不变）"); return; }
      for (const it of changed) {
        await api("/api/settings/service", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ key: it.key, value: drafts[it.key] }),
        });
      }
      setDrafts({});
      await load();
      notify("系统配置已保存，快递100 实时查询已生效（无需重启）");
    } catch (e) { notify((e as Error).message); } finally { setBusy(false); }
  }

  if (!user) return <EmptyState icon={<Settings2 />} title="登录后配置系统参数" text="这里集中管理启动服务的先决条件类密钥，例如快递100 实时查询所需的 KEY 与客户号。" action="请先登录" onAction={() => notify("请从右上角登录")} />;

  return <>
    <div className="page-intro"><span className="kicker">SYSTEM CONFIG</span><h2>系统配置</h2><p>把原本要写在后端 <code>.env</code> 并重启才能生效的密钥，改为在后台直接填写。DB 中的值优先于环境变量；清空某项即可回退到 <code>.env</code>。</p></div>
    <div className="profile-grid">
      <div className="member-card">
        <h3>快递100 实时查询密钥</h3>
        <p className="tt-hint">「内容管理 → 快递查询」中的「查询物流」按钮依赖此密钥。来自 <code>.env</code> 的已有配置也会在此显示，状态见「来源」。</p>
        {items.length === 0 ? <div className="tt-empty">加载中…</div> : items.map((it) => {
          const draft = drafts[it.key] ?? "";
          const showValue = it.secret ? (reveal[it.key] ? draft : it.masked || "（未配置）") : (draft || it.masked || "");
          const sourceLabel = it.source === "db" ? "后台" : it.source === "env" ? ".env" : "—";
          return (
            <div className="sc-row" key={it.key}>
              <label className="mini">{it.label} {it.configured ? <span className="sc-badge on">已配置 · 来源 {sourceLabel}</span> : <span className="sc-badge off">未配置</span>}</label>
              <div className="sc-input-wrap">
                <input
                  className="cs-input"
                  type={it.secret && !reveal[it.key] ? "password" : "text"}
                  placeholder={it.secret ? (it.configured ? "已保存，留空保持不变；输入新值覆盖" : it.placeholder) : it.placeholder}
                  value={it.secret ? (reveal[it.key] ? draft : (draft || "")) : (draft || "")}
                  onChange={(e) => setDraft(it.key, e.target.value)}
                  disabled={busy}
                />
                {it.secret ? <button className="button sm ghost" type="button" onClick={() => setReveal((p) => ({ ...p, [it.key]: !p[it.key] }))}>{reveal[it.key] ? "隐藏" : "显示"}</button> : null}
              </div>
              <p className="tt-hint">{it.hint}</p>
            </div>
          );
        })}
        <button className="button primary" disabled={busy} onClick={() => void save()}>{busy ? <LoaderCircle className="spin" size={16} /> : <Save size={16} />}保存系统配置</button>
      </div>

      <div className="member-card">
        <h3>实时查询状态</h3>
        <div className="sc-status">
          <span className={`status-pill ${expressConfigured ? "on" : ""}`}><i/>{expressConfigured === null ? "检测中…" : expressConfigured ? "快递100 已可用" : "未配置，物流查询为手动维护"}</span>
        </div>
        <p className="tt-hint">在「内容管理 → 快递查询」中录入运单号后，点击「查询物流」即可拉取实时轨迹。未配置时自动降级为手动维护状态，不影响使用。</p>
      </div>
    </div>
  </>;
}

function EmptyState({ icon,title,text,action,onAction }: {icon:React.ReactNode;title:string;text:string;action:string;onAction:()=>void}) { return <div className="empty"><div>{icon}</div><h2>{title}</h2><p>{text}</p><button className="button primary" onClick={onAction}>{action}</button></div>; }

function AuthModal({ onClose, onSuccess }: { onClose:()=>void; onSuccess:(u:User)=>void }) {
  const [mode,setMode]=useState<"login"|"register"|"reset">("login");
  const [resetStep,setResetStep]=useState<"email"|"verify">("email");
  const [username,setUsername]=useState("");const [password,setPassword]=useState("");
  const [email,setEmail]=useState("");const [phone,setPhone]=useState("");const [phoneRegion,setPhoneRegion]=useState("CN");const [code,setCode]=useState("");
  const [busy,setBusy]=useState(false);const [error,setError]=useState("");const [success,setSuccess]=useState("");
  function switchMode(next:"login"|"register"|"reset"){setMode(next);setError("");setSuccess("");setPassword("");}
  async function submit(){setBusy(true);setError("");setSuccess("");try{
    if(mode==="register"){
      await api("/api/auth/register",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({username,password,email:email.trim(),...(phone.trim()?{phone:phone.trim(),phone_region:phoneRegion}:{})})});
      setSuccess("注册成功，请使用新账号登录");setMode("login");setPassword("");setPhone("");return;
    }
    if(mode==="reset"){
      if(resetStep==="email"){await api("/api/auth/reset-password/send-code",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({email:email.trim()})});setResetStep("verify");setSuccess("验证码已发送到注册邮箱");return;}
      await api("/api/auth/reset-password",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({email:email.trim(),code:code.trim(),password})});setSuccess("密码重置成功，请重新登录");setMode("login");setResetStep("email");setPassword("");setCode("");return;
    }
    const result=await api<{token?:string}>("/api/auth/login",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({username,password})});if(!result.token)throw new Error("登录响应缺少令牌");
    setToken(result.token);document.cookie=`ink_session=${result.token}; path=/; max-age=${30*24*60*60}; SameSite=Lax`;const me=await api<User>("/api/auth/me");onSuccess(me);
  }catch(e){setError((e as Error).message)}finally{setBusy(false)}}
  const title=mode==="login"?"登录双屏后台":mode==="register"?"注册 小悟AI 账号":"重置密码";
  const disabled=busy||(mode==="login"&&(!username||!password))||(mode==="register"&&(!username||password.length<4||!email))||(mode==="reset"&&(!email||(resetStep==="verify"&&(!code||password.length<4))));
  return <Modal title={title} subtitle="账号、设备和绑定关系与原后台完全共用" onClose={onClose}>
    {mode!=="reset"?<div className="tabs"><button className={mode==="login"?"active":""} onClick={()=>switchMode("login")}>登录</button><button className={mode==="register"?"active":""} onClick={()=>switchMode("register")}>注册</button></div>:null}
    {mode!=="reset"?<><label>用户名<input autoFocus minLength={2} maxLength={30} autoComplete="username" value={username} onChange={e=>setUsername(e.target.value)} placeholder={mode==="register"?"2–30 个字符":"请输入用户名"}/></label><label>密码<input type="password" minLength={4} autoComplete={mode==="login"?"current-password":"new-password"} value={password} onChange={e=>setPassword(e.target.value)} onKeyDown={e=>e.key==="Enter"&&void submit()} placeholder="密码至少 4 位"/></label></>:null}
    {mode==="register"?<><label>注册邮箱 <b className="required">*</b><input type="email" autoComplete="email" value={email} onChange={e=>setEmail(e.target.value)} placeholder="用于找回密码"/></label><div className="phone-field"><label>地区<select value={phoneRegion} onChange={e=>setPhoneRegion(e.target.value)}><option value="CN">中国大陆 +86</option><option value="HK">中国香港 +852</option><option value="TW">中国台湾 +886</option><option value="SG">新加坡 +65</option><option value="US">美国 +1</option></select></label><label>手机号（可选）<input value={phone} onChange={e=>setPhone(e.target.value)} placeholder="可不填写"/></label></div></>:null}
    {mode==="reset"?<><label>注册邮箱<input autoFocus type="email" value={email} onChange={e=>setEmail(e.target.value)} placeholder="输入注册时使用的邮箱"/></label>{resetStep==="verify"?<><label>邮箱验证码<input value={code} onChange={e=>setCode(e.target.value)} placeholder="请输入验证码"/></label><label>新密码<input type="password" minLength={4} value={password} onChange={e=>setPassword(e.target.value)} placeholder="密码至少 4 位"/></label></>:null}</>:null}
    {error?<p className="form-error">{error}</p>:null}{success?<p className="form-success">{success}</p>:null}<button className="button primary full" disabled={disabled} onClick={()=>void submit()}>{busy?<LoaderCircle className="spin" size={16}/>:null}{mode==="login"?"登录":mode==="register"?"注册":resetStep==="email"?"发送验证码":"确认重置"}</button>
    <div className="auth-links">{mode==="login"?<button onClick={()=>switchMode("reset")}>忘记密码？</button>:mode==="reset"?<button onClick={()=>switchMode("login")}>返回登录</button>:null}</div><p className="modal-note">直接调用原后台认证接口，不创建新的用户体系。</p>
  </Modal>;
}

function PairModal({ onClose,onSuccess }:{onClose:()=>void;onSuccess:()=>void}) {
  const [tab,setTab]=useState<"code"|"mac">("code");const [value,setValue]=useState("");const [nickname,setNickname]=useState("");const [busy,setBusy]=useState(false);const [error,setError]=useState("");
  async function submit(){setBusy(true);setError("");try{if(tab==="code")await api("/api/claim/consume",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({pair_code:value.trim().toUpperCase()})});else await api("/api/user/devices",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({mac:value.trim().toUpperCase(),nickname})});onSuccess();}catch(e){setError((e as Error).message)}finally{setBusy(false)}}
  return <Modal title="绑定一台双屏设备" subtitle="沿用 小悟AI 安全绑定流程" onClose={onClose}><div className="tabs"><button className={tab==="code"?"active":""} onClick={()=>{setTab("code");setValue("")}}>配对码</button><button className={tab==="mac"?"active":""} onClick={()=>{setTab("mac");setValue("")}}>MAC 地址</button></div><label>{tab==="code"?"设备配对码":"设备 MAC"}<input autoFocus value={value} onChange={e=>setValue(e.target.value)} placeholder={tab==="code"?"例如 8K2WQ9":"例如 DC:DA:0C:44:6D:D4"}/></label>{tab==="mac"?<label>设备名称（可选）<input value={nickname} onChange={e=>setNickname(e.target.value)} placeholder="客厅双屏"/></label>:<div className="pair-tip"><ShieldCheck size={17}/><span>配对码显示在设备配网页或屏幕上；已有所有者时将发送加入申请。</span></div>}{error?<p className="form-error">{error}</p>:null}<button className="button primary full" disabled={busy||!value.trim()} onClick={()=>void submit()}>{busy?<LoaderCircle className="spin" size={16}/>:<Link2 size={16}/>}确认绑定</button></Modal>;
}

// ===== 内容管理：课程表 + 便签（参考 miaomiao 课程表 / 文字留言模型） =====
type CourseCell = { name: string; teacher?: string; room: string; weeks: string } | null;
type CoursePeriod = { start_hour: number; start_minute: number; end_hour: number; end_minute: number };
type BreakRow = { after: number; label: string; time: string };
type CourseTable = {
  enabled: boolean;
  period_count: number;
  show_days: number;
  semester_start: number;
  periods: CoursePeriod[];
  grid: CourseCell[][];
  school?: string;
  class_name?: string;
  breaks?: BreakRow[];
};
type Note = { id: string; text: string; color: string; sender: string; persistent: boolean; date: string; sender_type?: "parent" | "teacher" };
type HomeworkItem = { id: string; text: string; done: boolean; sender: string; date: string; sender_type?: "parent" | "teacher" };
type ReadingItem = { id: string; title: string; progress: string };
type AlbumPhoto = { id: string; url: string; caption: string };
type AlbumState = { enabled: boolean; photos: AlbumPhoto[] };

// 缴费台账：家庭固定开销（电费/水费/燃气费/伙食费/宽带费/大模型费用/其他），支持月付/年付、提前提醒、一键核销与缴费历史。
type BillCycle = "monthly" | "yearly"; // 月付 / 年付
type BillCategory = "电费" | "水费" | "燃气费" | "伙食费" | "宽带费" | "大模型费用" | "其他";
type BillPayment = { date: string; amount: number };
type BillItem = {
  id: string;
  category: BillCategory;
  name: string;
  amount: number;
  cycle: BillCycle;
  nextDue: string;
  remindDays: number;
  paid: boolean;
  history: BillPayment[];
  note?: string;
};

// 快递查询：家庭包裹物流台账（手动维护 + 可选对接快递100实时查询）。
type ExpressStatus = "ordered" | "transit" | "delivering" | "signed" | "exception"; // 已下单/运输中/派送中/已签收/问题件
type ExpressTrail = { time: string; context: string };
type ExpressItem = {
  id: string;
  company: string;
  trackingNo: string;
  item: string;
  status: ExpressStatus;
  trail: ExpressTrail[];
  updatedAt: string;
  note?: string;
  pickupCode?: string;
  pickupPoint?: string;
};
type FamilyEvent = { id: string; title: string; date: string; time?: string; member?: string; note?: string; category?: string; repeat?: string; reminder?: string };
type FamilyListItem = { id: string; text: string; done?: boolean; date?: string; member?: string };
type FamilyMember = { name: string; role: "admin" | "member" };
type ContentState = { courseTable: CourseTable; notes: Note[]; homework: HomeworkItem[]; reading: ReadingItem[]; album: AlbumState; bills: BillItem[]; express: ExpressItem[]; familySchedule: FamilyEvent[]; familyShopping: FamilyListItem[]; familyAnniversaries: FamilyEvent[]; familyMembers: FamilyMember[]; weatherCity: string };

const BILL_CATEGORIES: BillCategory[] = ["电费", "水费", "燃气费", "伙食费", "宽带费", "大模型费用", "其他"];
const EXPRESS_COMPANIES = ["顺丰", "京东", "中通", "圆通", "韵达", "申通", "邮政EMS", "极兔", "德邦", "其他"];
const EXPRESS_STATUS: { key: ExpressStatus; label: string }[] = [
  { key: "ordered", label: "已下单" },
  { key: "transit", label: "运输中" },
  { key: "delivering", label: "派送中" },
  { key: "signed", label: "已签收" },
  { key: "exception", label: "问题件" },
];
function expressStatusLabel(s: ExpressStatus) {
  return EXPRESS_STATUS.find((x) => x.key === s)?.label || s;
}
function dayDiff(a: string, b: string): number {
  const da = new Date(a + "T00:00:00").getTime();
  const db = new Date(b + "T00:00:00").getTime();
  return Math.round((db - da) / 86400000);
}
function advanceDate(dateStr: string, cycle: BillCycle): string {
  const d = new Date(dateStr + "T00:00:00");
  if (cycle === "yearly") d.setFullYear(d.getFullYear() + 1);
  else d.setMonth(d.getMonth() + 1);
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")}`;
}
function billStatus(b: BillItem, today: string) {
  if (b.paid && today < b.nextDue) return { key: "paid", label: "已缴 ✓", cls: "paid" } as const;
  if (today > b.nextDue) return { key: "overdue", label: "已逾期", cls: "overdue" } as const;
  const days = dayDiff(today, b.nextDue);
  if (days <= (b.remindDays ?? 3)) return { key: "soon", label: `${days} 天后到期`, cls: "soon" } as const;
  return { key: "pending", label: "待缴", cls: "pending" } as const;
}

const TT_DAYS = ["一", "二", "三", "四", "五", "六", "日"];
// A 屏内容模式（写入 config[sceneKey].a.mode，后端据此渲染 DUAL_A）
const A_MODES: { key: string; label: string }[] = [
  { key: "notes", label: "便签" },
  { key: "homework", label: "今日作业" },
  { key: "reading", label: "阅读任务" },
  { key: "album", label: "相册" },
];
const TT_COLORS: { key: string; label: string; bg: string; bd: string }[] = [
  { key: "yellow", label: "黄", bg: "#fdf3c7", bd: "#e6cf6b" },
  { key: "pink", label: "粉", bg: "#fde4ec", bd: "#f0a8c4" },
  { key: "blue", label: "蓝", bg: "#dcebfb", bd: "#9cc4ee" },
  { key: "green", label: "绿", bg: "#dff2e4", bd: "#a9d8b8" },
  { key: "white", label: "白", bg: "#ffffff", bd: "#deded5" },
];

function ttStandardPeriods(): CoursePeriod[] {
  return [
    { start_hour: 8, start_minute: 0, end_hour: 8, end_minute: 45 },
    { start_hour: 8, start_minute: 55, end_hour: 9, end_minute: 40 },
    { start_hour: 10, start_minute: 0, end_hour: 10, end_minute: 45 },
    { start_hour: 10, start_minute: 55, end_hour: 11, end_minute: 40 },
    { start_hour: 14, start_minute: 0, end_hour: 14, end_minute: 45 },
    { start_hour: 14, start_minute: 55, end_hour: 15, end_minute: 40 },
    { start_hour: 16, start_minute: 0, end_hour: 16, end_minute: 45 },
    { start_hour: 16, start_minute: 55, end_hour: 17, end_minute: 40 },
    { start_hour: 19, start_minute: 0, end_hour: 19, end_minute: 45 },
    { start_hour: 19, start_minute: 55, end_hour: 20, end_minute: 40 },
  ];
}

// 中学全天模板（参照真实课表：成都七中初中部 10 节 + 眼保健操/大课间/午餐午休固定行）
function ttMiddleSchoolTemplate(): { periods: CoursePeriod[]; breaks: BreakRow[] } {
  return {
    periods: [
      { start_hour: 8, start_minute: 0, end_hour: 8, end_minute: 40 },
      { start_hour: 8, start_minute: 50, end_hour: 9, end_minute: 30 },
      { start_hour: 9, start_minute: 45, end_hour: 10, end_minute: 25 },
      { start_hour: 11, start_minute: 0, end_hour: 11, end_minute: 35 },
      { start_hour: 11, start_minute: 35, end_hour: 12, end_minute: 25 },
      { start_hour: 14, start_minute: 10, end_hour: 14, end_minute: 50 },
      { start_hour: 15, start_minute: 5, end_hour: 15, end_minute: 45 },
      { start_hour: 16, start_minute: 0, end_hour: 16, end_minute: 40 },
      { start_hour: 16, start_minute: 55, end_hour: 17, end_minute: 35 },
      { start_hour: 17, start_minute: 50, end_hour: 18, end_minute: 30 },
    ],
    breaks: [
      { after: 2, label: "眼保健操（上午）", time: "9:30~9:45" },
      { after: 3, label: "大课间体育锻炼", time: "10:25~11:00" },
      { after: 5, label: "午餐午休；课间十分钟", time: "12:25~14:10" },
      { after: 7, label: "眼保健操（下午）", time: "15:45~16:00" },
    ],
  };
}

function makeDefaultContent(): ContentState {
  return {
    courseTable: {
      enabled: true,
      period_count: 10,
      show_days: 5,
      semester_start: 0,
      periods: ttStandardPeriods(),
      grid: Array.from({ length: 7 }, () => Array(10).fill(null)),
      breaks: [],
    },
    notes: [],
    homework: [],
    reading: [],
    album: { enabled: true, photos: [] },
    bills: [],
    express: [],
    familySchedule: [],
    familyShopping: [],
    familyAnniversaries: [],
    familyMembers: [],
    weatherCity: "",
  };
}

function semesterStartToUnix(dateStr: string): number {
  if (!dateStr) return 0;
  const d = new Date(dateStr + "T00:00:00");
  const dow = d.getDay();
  const off = dow === 0 ? -6 : 1 - dow;
  d.setDate(d.getDate() + off);
  return Math.floor(d.getTime() / 1000);
}
function unixToDateStr(ts: number): string {
  if (!ts) return "";
  const d = new Date(ts * 1000);
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")}`;
}
function ensureGrid(ct: CourseTable): CourseTable {
  const grid: CourseCell[][] = ct.grid.map((r) => r.slice());
  while (grid.length < 7) grid.push([]);
  for (let d = 0; d < 7; d++) {
    const row = grid[d].slice(0, ct.period_count);
    while (row.length < ct.period_count) row.push(null);
    grid[d] = row;
  }
  return { ...ct, grid };
}

function ContentStudio({ content, setContent, save, savedAt, notify, apply, hasDevice, deviceMac, screen, sceneKey, aMode, bMode }: { content: ContentState; setContent: React.Dispatch<React.SetStateAction<ContentState>>; save: () => void; savedAt: string; notify: (m: string) => void; apply: () => void; hasDevice: boolean; deviceMac: string; screen: { size: "4.2" | "5.83"; colors: 2 | 3 | 4 }; sceneKey: SceneKey; aMode: string; bMode: string }) {
  const { weatherPreview, weatherLoading, weatherError } = useDualWeatherPreview(sceneKey === "family" && aMode === "family-weather", deviceMac, content.weatherCity);
  const [tab, setTab] = useState<"course" | "notes" | "homework" | "reading" | "album" | "bills" | "express" | "schedule" | "shopping" | "anniversaries" | "history">("course");
  return <>
    <div className="page-intro row"><div><span className="kicker">CONTENT STUDIO</span><h2>内容管理</h2><p>在这里维护各模式的数据；屏幕模式决定展示哪些数据。家庭 A 屏「快递与缴费」由「快递/取件码」和「缴费台账」两项内容合并展示。</p></div><div className="save-state"><ShieldCheck size={17} />{savedAt ? `模式保存于 ${savedAt}` : "配置尚未保存"}</div></div>
    <DualPreview content={content} screen={screen} sceneKey={sceneKey} aMode={aMode} bMode={bMode} weatherPreview={weatherPreview} weatherLoading={weatherLoading} weatherError={weatherError} />
    <div className="content-tabs">
      <button className={tab === "course" ? "active" : ""} onClick={() => setTab("course")}><CalendarDays size={16} />课程表</button>
      <button className={tab === "notes" ? "active" : ""} onClick={() => setTab("notes")}><StickyNote size={16} />便签</button>
      <button className={tab === "homework" ? "active" : ""} onClick={() => setTab("homework")}><ListChecks size={16} />今日作业</button>
      <button className={tab === "reading" ? "active" : ""} onClick={() => setTab("reading")}><BookOpen size={16} />阅读任务</button>
      <button className={tab === "album" ? "active" : ""} onClick={() => setTab("album")}><ImageIcon size={16} />相册</button>
      <button className={tab === "bills" ? "active" : ""} onClick={() => setTab("bills")}><WalletCards size={16} />缴费台账</button>
      <button className={tab === "express" ? "active" : ""} onClick={() => setTab("express")}><Truck size={16} />快递/取件码</button>
      <button className={tab === "schedule" ? "active" : ""} onClick={() => setTab("schedule")}><CalendarDays size={16} />家庭日程</button>
      <button className={tab === "shopping" ? "active" : ""} onClick={() => setTab("shopping")}><ListChecks size={16} />采购清单</button>
      <button className={tab === "anniversaries" ? "active" : ""} onClick={() => setTab("anniversaries")}><Bell size={16} />纪念日</button>
      <button className={tab === "history" ? "active" : ""} onClick={() => setTab("history")}><History size={16} />历史归档</button>
    </div>
    {tab === "course"
      ? <CourseTableEditor value={content.courseTable} onChange={(ct) => setContent({ ...content, courseTable: ct })} notify={notify} />
      : tab === "notes"
        ? <NotesEditor value={content.notes} onChange={(notes) => setContent({ ...content, notes })} notify={notify} />
        : tab === "homework"
          ? <HomeworkEditor value={content.homework} onChange={(homework) => setContent({ ...content, homework })} notify={notify} />
          : tab === "reading"
            ? <ReadingEditor value={content.reading} onChange={(reading) => setContent({ ...content, reading })} notify={notify} />
            : tab === "album"
              ? <AlbumEditor value={content.album} onChange={(album) => setContent({ ...content, album })} notify={notify} />
              : tab === "bills"
                ? <BillLedgerEditor value={content.bills} onChange={(bills) => setContent({ ...content, bills })} notify={notify} />
                : tab === "express"
                  ? <ExpressEditor value={content.express} onChange={(express) => setContent({ ...content, express })} notify={notify} />
                  : tab === "schedule"
                    ? <FamilyEventsEditor title="家庭日程" members={content.familyMembers} value={content.familySchedule} onChange={(familySchedule) => setContent({ ...content, familySchedule })} />
                    : tab === "shopping"
                      ? <FamilyListEditor title="家庭采购清单" value={content.familyShopping} onChange={(familyShopping) => setContent({ ...content, familyShopping })} />
                      : tab === "anniversaries"
                        ? <FamilyEventsEditor title="家庭纪念日" value={content.familyAnniversaries} onChange={(familyAnniversaries) => setContent({ ...content, familyAnniversaries })} />
                        : <HistoryArchive content={content} setContent={setContent} notify={notify} />}
    <div className="sticky-save"><div><b>保存双屏模式</b><span>课程表、便签、作业与阅读任务一并写入本地模式，与场景配置共用。</span></div><div className="save-actions"><button className="button primary" onClick={save}><Save size={16} />保存模式</button><button className="button soft" onClick={apply} disabled={!hasDevice} title={hasDevice ? "下发到所选设备" : "请先在设备管理绑定设备"}><Cloud size={16} />应用到设备</button></div></div>
  </>;
}

function CourseTableEditor({ value, onChange, notify }: { value: CourseTable; onChange: (ct: CourseTable) => void; notify: (m: string) => void }) {
  const [edit, setEdit] = useState<{ day: number; period: number } | null>(null);
  const [name, setName] = useState("");
  const [teacher, setTeacher] = useState("");
  const [room, setRoom] = useState("");
  const [weeks, setWeeks] = useState("");
  const cols = value.show_days || 5;

  // 按行添加状态与逻辑
  const [addDay, setAddDay] = useState(0);
  const [selPeriod, setSelPeriod] = useState(0);
  const [addName, setAddName] = useState("");
  const [addTeacher, setAddTeacher] = useState("");
  const [addRoom, setAddRoom] = useState("");
  const [addWeeks, setAddWeeks] = useState("");
  function addRow() {
    const n = addName.trim();
    if (!n) { notify("请先填课程名称"); return; }
    if (value.period_count === 0) { notify("请先添加或套用节次"); return; }
    const grid = value.grid.map((r) => r.slice());
    while (grid.length < 7) grid.push([]);
    grid[addDay][selPeriod] = { name: n, teacher: addTeacher.trim(), room: addRoom.trim(), weeks: addWeeks.trim() };
    onChange({ ...value, grid });
    setAddName(""); setAddTeacher(""); setAddRoom(""); setAddWeeks("");
    notify(`已添加：周${TT_DAYS[addDay]} 第${selPeriod + 1}节 ${n}`);
  }
  function removeRow(d: number, p: number) {
    const grid = value.grid.map((r) => r.slice());
    if (!grid[d]) grid[d] = [];
    grid[d][p] = null;
    onChange({ ...value, grid });
  }
  const courseRows = (() => {
    const out: { day: number; period: number; name: string; teacher: string; room: string; weeks: string }[] = [];
    for (let d = 0; d < cols; d++) {
      for (let p = 0; p < value.period_count; p++) {
        const c = value.grid[d]?.[p];
        if (c && c.name) out.push({ day: d, period: p, name: c.name, teacher: c.teacher || "", room: c.room || "", weeks: c.weeks || "" });
      }
    }
    return out;
  })();

  function patch(next: Partial<CourseTable>) { onChange({ ...value, ...next }); }

  function addPeriod() {
    const last = value.periods[value.periods.length - 1];
    let sh = last ? last.end_hour : 8;
    let sm = last ? (last.end_minute + 10) % 60 : 0;
    if (last && last.end_minute + 10 >= 60) sh += 1;
    const total = sm + 45;
    const periods = [...value.periods, { start_hour: sh, start_minute: sm, end_hour: total >= 60 ? sh + 1 : sh, end_minute: total >= 60 ? total - 60 : total }];
    patch(ensureGrid({ ...value, periods, period_count: periods.length }));
  }
  function removePeriod(i: number) {
    const periods = value.periods.filter((_, idx) => idx !== i);
    patch(ensureGrid({ ...value, periods, period_count: periods.length }));
  }
  function updatePeriod(i: number, k: keyof CoursePeriod, v: number) {
    const periods = value.periods.map((p, idx) => (idx === i ? { ...p, [k]: v } : p));
    patch({ ...value, periods });
  }
  function applyTemplate() {
    const periods = ttStandardPeriods();
    patch(ensureGrid({ ...value, periods, period_count: periods.length }));
    notify("已套用标准 10 节模板");
  }
  function applyZhongxue() {
    const t = ttMiddleSchoolTemplate();
    patch(ensureGrid({ ...value, periods: t.periods, period_count: t.periods.length, breaks: t.breaks }));
    notify("已套用中学全天模板（10 节 + 固定行）");
  }
  function addBreak() {
    const breaks = [...(value.breaks || []), { after: Math.max(1, Math.min(value.period_count, 2)), label: "", time: "" }];
    patch({ ...value, breaks });
  }
  function updateBreak(i: number, k: keyof BreakRow, v: string | number) {
    const breaks = (value.breaks || []).map((b, idx) => (idx === i ? { ...b, [k]: v } : b));
    patch({ ...value, breaks });
  }
  function removeBreak(i: number) {
    patch({ ...value, breaks: (value.breaks || []).filter((_, idx) => idx !== i) });
  }
  function openCell(d: number, p: number) {
    const cell = value.grid[d]?.[p];
    setName(cell?.name ?? "");
    setTeacher(cell?.teacher ?? "");
    setRoom(cell?.room ?? "");
    setWeeks(cell?.weeks ?? "");
    setEdit({ day: d, period: p });
  }
  function saveCell() {
    if (!edit) return;
    const grid = value.grid.map((r) => r.slice());
    while (grid.length < 7) grid.push([]);
    const n = name.trim();
    grid[edit.day][edit.period] = n ? { name: n, teacher: teacher.trim(), room: room.trim(), weeks: weeks.trim() } : null;
    onChange({ ...value, grid });
    setEdit(null);
  }
  function deleteCell() {
    if (!edit) return;
    const grid = value.grid.map((r) => r.slice());
    if (!grid[edit.day]) grid[edit.day] = [];
    grid[edit.day][edit.period] = null;
    onChange({ ...value, grid });
    setEdit(null);
  }
  function clearAll() {
    if (!confirm("确定清空所有课程？")) return;
    const grid = Array.from({ length: 7 }, () => Array(value.period_count).fill(null));
    onChange({ ...value, grid });
    notify("已清空课程表");
  }
  function exportJson() {
    const blob = new Blob([JSON.stringify(value, null, 2)], { type: "application/json" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = "course_table_" + new Date().toISOString().slice(0, 10) + ".json";
    a.click();
    URL.revokeObjectURL(a.href);
    notify("课程表已导出");
  }
  function importJson(input: HTMLInputElement) {
    if (!input.files || input.files.length === 0) return;
    input.files[0].text().then((text) => {
      try {
        const data = JSON.parse(text);
        if (!Array.isArray(data.periods) || !Array.isArray(data.grid)) { notify("JSON 格式无效"); return; }
        onChange(ensureGrid({
          enabled: !!data.enabled,
          period_count: data.periods.length,
          show_days: data.show_days || 5,
          semester_start: data.semester_start || 0,
          periods: data.periods,
          grid: data.grid,
          school: data.school || "",
          class_name: data.class_name || "",
          breaks: Array.isArray(data.breaks) ? data.breaks : [],
        }));
        notify("导入成功，记得点保存");
      } catch { notify("导入失败：JSON 解析错误"); }
    });
    input.value = "";
  }

  return <>
    <div className="card-pad">
      <div className="ct-controls">
        <label className="ct-toggle"><input type="checkbox" checked={value.enabled} onChange={(e) => patch({ enabled: e.target.checked })} /><span className="ct-track"><span className="ct-thumb" /></span><b>启用课程表</b></label>
        <div className="ct-inline">
          <label>学期开始<input className="cs-input" type="date" value={unixToDateStr(value.semester_start)} onChange={(e) => patch({ semester_start: semesterStartToUnix(e.target.value) })} /></label>
          <label>显示天数<select className="cs-input" value={value.show_days} onChange={(e) => patch(ensureGrid({ ...value, show_days: parseInt(e.target.value) || 5 }))}><option value={5}>周一至周五</option><option value={7}>周一至周日</option></select></label>
          <label>学校<input className="cs-input" type="text" value={value.school || ""} onChange={(e) => patch({ school: e.target.value })} placeholder="选填" /></label>
          <label>班级<input className="cs-input" type="text" value={value.class_name || ""} onChange={(e) => patch({ class_name: e.target.value })} placeholder="如：初三16班" /></label>
        </div>
      </div>

      <div className="tt-periods">
        <div className="tt-periods-head"><span>节次时间</span><div><button className="button sm ghost" onClick={addPeriod}>+ 节次</button><button className="button sm ghost" onClick={applyTemplate}>标准模板</button><button className="button sm ghost" onClick={applyZhongxue}>中学全天模板</button></div></div>
        <div className="tt-period-list">
          {value.periods.map((p, i) => (
            <div className="tt-period-row" key={i}>
              <span>第{i + 1}节</span>
              <input className="cs-input" type="number" min={0} max={23} value={p.start_hour} onChange={(e) => updatePeriod(i, "start_hour", parseInt(e.target.value) || 0)} />:
              <input className="cs-input" type="number" min={0} max={59} value={p.start_minute} onChange={(e) => updatePeriod(i, "start_minute", parseInt(e.target.value) || 0)} />
              <span className="dash">—</span>
              <input className="cs-input" type="number" min={0} max={23} value={p.end_hour} onChange={(e) => updatePeriod(i, "end_hour", parseInt(e.target.value) || 0)} />:
              <input className="cs-input" type="number" min={0} max={59} value={p.end_minute} onChange={(e) => updatePeriod(i, "end_minute", parseInt(e.target.value) || 0)} />
              <button className="button sm ghost danger" onClick={() => removePeriod(i)} aria-label="删除节次">×</button>
            </div>
          ))}
          {value.periods.length === 0 ? <div className="tt-empty">暂无节次，点「+ 节次」或「套用模板」。</div> : null}
        </div>
      </div>

      <div className="tt-periods">
        <div className="tt-periods-head"><span>固定行（课间操 / 午休等）</span><div><button className="button sm ghost" onClick={addBreak}>+ 固定行</button></div></div>
        <div className="tt-period-list">
          {(value.breaks || []).map((b, i) => (
            <div className="tt-period-row" key={i}>
              <select className="cs-input" value={b.after} onChange={(e) => updateBreak(i, "after", parseInt(e.target.value) || 0)}>{Array.from({ length: value.period_count }).map((_, x) => <option key={x} value={x + 1}>第{x + 1}节后</option>)}</select>
              <input className="cs-input" type="text" value={b.label} onChange={(e) => updateBreak(i, "label", e.target.value)} placeholder="活动名称，如 大课间体育锻炼" />
              <input className="cs-input" type="text" value={b.time} onChange={(e) => updateBreak(i, "time", e.target.value)} placeholder="时间，如 10:25~11:00" />
              <button className="button sm ghost danger" onClick={() => removeBreak(i)} aria-label="删除固定行">×</button>
            </div>
          ))}
          {(value.breaks || []).length === 0 ? <div className="tt-empty">无固定行。可用「中学全天模板」一键带入眼保健操 / 大课间 / 午休。</div> : null}
        </div>
      </div>

      <div className="card-pad tt-addcard">
        <div className="tt-grid-head"><span>按行添加课程</span><span className="tt-hint">填一行即落到课表，不用再点格子</span></div>
        <div className="tt-addrow">
          <label>星期<select className="cs-input" value={addDay} onChange={(e) => setAddDay(parseInt(e.target.value) || 0)}>{TT_DAYS.slice(0, cols).map((d, i) => <option key={i} value={i}>周{d}</option>)}</select></label>
          <label>节次<select className="cs-input" value={selPeriod} onChange={(e) => setSelPeriod(parseInt(e.target.value) || 0)} disabled={value.period_count === 0}>{Array.from({ length: value.period_count }).map((_, i) => <option key={i} value={i}>第{i + 1}节</option>)}</select></label>
          <label>课程名称<input className="cs-input" type="text" maxLength={39} value={addName} onChange={(e) => setAddName(e.target.value)} placeholder="如：高等数学" /></label>
          <label>老师<input className="cs-input" type="text" maxLength={19} value={addTeacher} onChange={(e) => setAddTeacher(e.target.value)} placeholder="选填，如 戴蔚华" /></label>
          <label>教室<input className="cs-input" type="text" maxLength={19} value={addRoom} onChange={(e) => setAddRoom(e.target.value)} placeholder="如：A101" /></label>
          <label>周次<input className="cs-input" type="text" value={addWeeks} onChange={(e) => setAddWeeks(e.target.value)} placeholder="1-16 / 留空=每周" /></label>
          <button className="button" onClick={addRow} disabled={value.period_count === 0}><Plus size={16} />添加</button>
        </div>
        {value.period_count === 0 ? <div className="tt-empty">还没节次，先用上方「套用模板」或「+ 节次」。</div> : null}
      </div>

      <div className="tt-grid-head"><span>课程安排（点格子也可编辑）</span>
        <div className="tt-grid-actions">
          <button className="button sm ghost danger" onClick={clearAll}>清空</button>
          <label className="button sm ghost" style={{ cursor: "pointer" }}>导入<input type="file" accept=".json" style={{ display: "none" }} onChange={(e) => importJson(e.target as HTMLInputElement)} /></label>
          <button className="button sm ghost" onClick={exportJson}>导出</button>
        </div>
      </div>
      <div className="tt-scroll">
        <table className="tt-table">
          <thead><tr><th className="period-col">节</th>{TT_DAYS.slice(0, cols).map((d, i) => <th key={i}>周{d}</th>)}</tr></thead>
          <tbody>
            {Array.from({ length: value.period_count }).map((_, p) => {
              const pd = value.periods[p];
              const t = pd ? `${String(pd.start_hour).padStart(2, "0")}:${String(pd.start_minute).padStart(2, "0")}` : "";
              const cells: React.ReactNode[] = [
                <tr key={p}>
                  <td className="period-col">{p + 1}<small>{t}</small></td>
                  {Array.from({ length: cols }).map((_, d) => {
                    const c = value.grid[d]?.[p];
                    return (
                      <td key={d} className={c?.name ? "filled" : "empty"} onClick={() => openCell(d, p)}>
                        {c?.name ? <><b>{c.name}</b>{c.teacher ? <small>（{c.teacher}）</small> : null}{c.room ? <small>{c.room}</small> : null}{c.weeks ? <em>{c.weeks}</em> : null}</> : <span className="plus">+</span>}
                      </td>
                    );
                  })}
                </tr>,
              ];
              const br = (value.breaks || []).find((b) => b.after === p + 1);
              if (br) cells.push(<tr className="tt-break" key={p + "-break"}><td colSpan={cols + 1}>{br.label || "固定行"}{br.time ? <small>{br.time}</small> : null}</td></tr>);
              return cells;
            })}
          </tbody>
        </table>
      </div>
      <div className="card-pad">
        <div className="tt-grid-head"><span>已排课程清单（{courseRows.length}）</span></div>
        {courseRows.length === 0 ? <div className="tt-empty">还没有课程，用上面的「按行添加」或点格子填写。</div> : (
          <div className="tt-rowlist">
            {courseRows.map((r) => (
              <div className="tt-rowitem" key={r.day + "-" + r.period}>
                <span className="tt-rd">周{TT_DAYS[r.day]}</span>
                <span className="tt-rp">第{r.period + 1}节</span>
                <span className="tt-rn">{r.name}</span>
                {r.teacher ? <span className="tt-rt">（{r.teacher}）</span> : null}
                <span className="tt-rr">{r.room}</span>
                <span className="tt-rw">{r.weeks || "每周"}</span>
                <button className="button sm ghost danger" onClick={() => removeRow(r.day, r.period)} aria-label="删除"><Trash2 size={14} /></button>
              </div>
            ))}
          </div>
        )}
      </div>
    </div>

    {edit ? (
      <div className="modal-backdrop" onMouseDown={(e) => e.currentTarget === e.target && setEdit(null)}>
        <div className="modal">
          <button className="modal-close" onClick={() => setEdit(null)}><X size={18} /></button>
          <div className="modal-brand">悟</div>
          <h2>{`周${TT_DAYS[edit.day]} 第${edit.period + 1}节`}</h2>
          <p>填写课程信息，留空即删除该格。</p>
          <label>课程名称<input className="cs-input" type="text" maxLength={39} value={name} onChange={(e) => setName(e.target.value)} placeholder="如：高等数学" /></label>
          <label>老师<input className="cs-input" type="text" maxLength={19} value={teacher} onChange={(e) => setTeacher(e.target.value)} placeholder="选填，如 戴蔚华" /></label>
          <label>教室<input className="cs-input" type="text" maxLength={19} value={room} onChange={(e) => setRoom(e.target.value)} placeholder="如：A101" /></label>
          <label>上课周次<input className="cs-input" type="text" value={weeks} onChange={(e) => setWeeks(e.target.value)} placeholder="如 1-16 或 1,3,5 或留空=每周" /></label>
          <div className="modal-body" style={{ display: "flex", gap: 8, justifyContent: "flex-end", padding: 0 }}>
            {name ? <button className="button sm ghost danger" onClick={deleteCell}>删除</button> : null}
            <button className="button sm ghost" onClick={() => setEdit(null)}>取消</button>
            <button className="button sm" onClick={saveCell}>确定</button>
          </div>
        </div>
      </div>
    ) : null}
  </>;
}

function NotesEditor({ value, onChange, notify }: { value: Note[]; onChange: (n: Note[]) => void; notify: (m: string) => void }) {
  const [text, setText] = useState("");
  const [color, setColor] = useState("yellow");
  const [sender, setSender] = useState("家长");
  const [persistent, setPersistent] = useState(false);
  const today = new Date().toISOString().slice(0, 10);
  const mk = () => Date.now().toString(36) + Math.random().toString(36).slice(2, 6);
  function add() {
    const t = text.trim();
    if (!t) { notify("便签内容不能为空"); return; }
    onChange([...value, { id: mk(), text: t, color, sender, sender_type: sender === "老师" ? "teacher" : "parent", persistent, date: today }]);
    setText("");
    notify("便签已添加");
  }
  function remove(id: string) { onChange(value.filter((n) => n.id !== id)); }
  function edit(id: string, patchNote: Partial<Note>) { onChange(value.map((n) => (n.id === id ? { ...n, ...patchNote } : n))); }
  return <>
    <div className="card-pad">
      <div className="note-compose">
        <textarea className="cs-input" value={text} maxLength={120} onChange={(e) => setText(e.target.value)} placeholder="输入便签内容…（最多 120 字）" />
        <div className="note-compose-foot">
          <div className="color-dots">
            {TT_COLORS.map((c) => <button key={c.key} type="button" className={color === c.key ? "active" : ""} style={{ background: c.bg, borderColor: c.bd }} onClick={() => setColor(c.key)} aria-label={c.label} />)}
          </div>
          <label className="mini">发送人<select className="cs-input" value={sender} onChange={(e) => setSender(e.target.value)}><option value="家长">家长</option><option value="老师">老师</option></select></label>
          <label className="mini">持续<select className="cs-input" value={persistent ? "1" : "0"} onChange={(e) => setPersistent(e.target.value === "1")}><option value="0">单次(当天)</option><option value="1">持续</option></select></label>
          <button className="button" onClick={add}><Plus size={16} />添加便签</button>
        </div>
      </div>
      <div className="note-list">
        {value.length === 0 ? <div className="tt-empty">还没有便签，先在上面添加一条。</div> : value.map((n) => {
          const col = TT_COLORS.find((c) => c.key === n.color) || TT_COLORS[0];
          return (
            <div className="note-card" key={n.id} style={{ background: col.bg, borderColor: col.bd }}>
              <textarea className="cs-input note-text" value={n.text} maxLength={120} onChange={(e) => edit(n.id, { text: e.target.value })} />
              <div className="note-card-foot">
                <div className="color-dots sm">
                  {TT_COLORS.map((c) => <button key={c.key} type="button" className={n.color === c.key ? "active" : ""} style={{ background: c.bg, borderColor: c.bd }} onClick={() => edit(n.id, { color: c.key })} aria-label={c.label} />)}
                </div>
                <label className="mini">来源<select className="cs-input" value={n.sender || "家长"} onChange={(e) => edit(n.id, { sender: e.target.value, sender_type: e.target.value === "老师" ? "teacher" : "parent" })}><option value="家长">家长</option><option value="老师">老师</option></select></label>
                <label className="mini">持续<input type="checkbox" checked={!!n.persistent} onChange={(e) => edit(n.id, { persistent: e.target.checked })} /></label>
                <button className="button sm ghost danger" onClick={() => remove(n.id)} aria-label="删除便签"><Trash2 size={14} /></button>
              </div>
            </div>
          );
        })}
      </div>
      <p className="tt-hint" style={{ marginTop: 8 }}>「持续」便签会一直显示直到手动删除；「单次」次日 0 点自动归档。老师便签在设备端以「师·」前缀区分。</p>
    </div>
  </>;
}

/** 前端压缩图片：最长边 maxEdge，输出 jpeg，降低上传体积与 OCR 耗时 */
function compressImage(file: File, maxEdge: number, quality: number): Promise<Blob> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onerror = () => reject(new Error("读取图片失败"));
    reader.onload = () => {
      const img = new Image();
      img.onerror = () => reject(new Error("图片解析失败"));
      img.onload = () => {
        const scale = Math.min(1, maxEdge / Math.max(img.width, img.height));
        const w = Math.round(img.width * scale);
        const h = Math.round(img.height * scale);
        const canvas = document.createElement("canvas");
        canvas.width = w; canvas.height = h;
        const ctx = canvas.getContext("2d");
        if (!ctx) return reject(new Error("无法创建画布"));
        ctx.drawImage(img, 0, 0, w, h);
        canvas.toBlob((b) => (b ? resolve(b) : reject(new Error("压缩失败"))), "image/jpeg", quality);
      };
      img.src = reader.result as string;
    };
    reader.readAsDataURL(file);
  });
}

function HomeworkEditor({ value, onChange, notify }: { value: HomeworkItem[]; onChange: (h: HomeworkItem[]) => void; notify: (m: string) => void }) {
  const [text, setText] = useState("");
  const [paste, setPaste] = useState("");
  const [importMode, setImportMode] = useState<"append" | "overwrite">("append");
  const [sender, setSender] = useState("家长");
  const [busyOcr, setBusyOcr] = useState(false);
  const fileRef = useRef<HTMLInputElement>(null);
  const today = new Date().toISOString().slice(0, 10);
  const mk = () => Date.now().toString(36) + Math.random().toString(36).slice(2, 6);
  function add() {
    const t = text.trim();
    if (!t) { notify("作业内容不能为空"); return; }
    onChange([...value, { id: mk(), text: t, done: false, sender, sender_type: sender === "老师" ? "teacher" : "parent", date: today }]);
    setText("");
    notify("作业已添加");
  }
  function importBatch() {
    const lines = paste.split(/\r?\n/).map((s) => s.trim()).filter(Boolean);
    if (!lines.length) { notify("请先粘贴作业文本"); return; }
    const st: "parent" | "teacher" = sender === "老师" ? "teacher" : "parent";
    const items = lines.map((t) => ({ id: mk(), text: t, done: false, sender, sender_type: st, date: today }));
    if (importMode === "overwrite") {
      const kept = value.filter((x) => x.date && x.date !== today); // 清掉当天，写入新作业
      onChange([...kept, ...items]);
      notify(`已覆盖为 ${items.length} 条新作业`);
    } else {
      onChange([...value, ...items]);
      notify(`已追加 ${items.length} 条作业`);
    }
    setPaste("");
  }
  async function onOcrFile(e: React.ChangeEvent<HTMLInputElement>) {
    const f = e.target.files?.[0];
    e.target.value = ""; // 允许重复选同一张
    if (!f) return;
    try {
      setBusyOcr(true);
      const blob = await compressImage(f, 1600, 0.85);
      const fd = new FormData();
      fd.append("file", blob, f.name || "homework.jpg");
      const res = await api<{ lines: string[] }>("/api/ocr/homework", { method: "POST", body: fd });
      const joined = (res.lines || []).join("\n").trim();
      if (!joined) {
        notify("未识别到作业，换一张更清晰的照片试试");
      } else {
        setPaste((prev) => (prev ? prev + "\n" : "") + joined);
        notify(`已识别 ${res.lines.length} 条，请核对后再导入`);
      }
    } catch (err) {
      notify((err as Error).message || "识别失败");
    } finally {
      setBusyOcr(false);
    }
  }
  return <>
    <div className="card-pad">
      <div className="hw-import">
        <label>批量粘贴导入<textarea className="cs-input" value={paste} onChange={(e) => setPaste(e.target.value)} placeholder={"直接粘贴微信群/班级群作业，支持换行，每行一条：\n数学：P42 1-12题\n英语：背诵Unit8单词"} /></label>
        <div className="hw-import-foot">
          <label className="mini">方式<select className="cs-input" value={importMode} onChange={(e) => setImportMode(e.target.value as "append" | "overwrite")}><option value="append">追加（推荐）</option><option value="overwrite">覆盖当天</option></select></label>
          <label className="mini">发送人<select className="cs-input" value={sender} onChange={(e) => setSender(e.target.value)}><option value="家长">家长</option><option value="老师">老师</option></select></label>
          <button className="button" onClick={importBatch}><Plus size={16} />导入作业</button>
        </div>
        <div className="hw-import-foot" style={{ marginTop: 8 }}>
          <button className="button light" onClick={() => fileRef.current?.click()} disabled={busyOcr}>
            {busyOcr ? <LoaderCircle className="spin" size={16} /> : <ScanText size={16} />}
            {busyOcr ? "识别中…" : "上传图片识别"}
          </button>
          <span className="muted" style={{ fontSize: 12, alignSelf: "center" }}>
            拍照/截图黑板作业，自动识别成文字（需配置阿里云视觉模型 Key）
          </span>
        </div>
        <input ref={fileRef} type="file" accept="image/*" hidden onChange={onOcrFile} />
      </div>
      <div className="note-compose">
        <input className="cs-input" value={text} maxLength={60} onChange={(e) => setText(e.target.value)}
          placeholder="或单条输入，如：数学口算第 12 页（最多 60 字）"
          onKeyDown={(e) => { if (e.key === "Enter") add(); }} />
        <div className="note-compose-foot"><span />
          <button className="button" onClick={add}><Plus size={16} />添加作业</button>
        </div>
      </div>
      <div className="hw-list">
        {value.length === 0 ? <div className="tt-empty">还没有作业，先粘贴或添加一条。</div> : value.map((it) => (
          <div className="hw-row" key={it.id}>
            <input type="checkbox" checked={it.done} onChange={(e) => onChange(value.map((x) => (x.id === it.id ? { ...x, done: e.target.checked } : x)))} aria-label="标记完成" />
            <input className="cs-input hw-text" value={it.text} maxLength={60}
              onChange={(e) => onChange(value.map((x) => (x.id === it.id ? { ...x, text: e.target.value } : x)))}
              style={it.done ? { textDecoration: "line-through", opacity: 0.55 } : undefined} />
            <select className="cs-input hw-sender" value={it.sender || "家长"} onChange={(e) => onChange(value.map((x) => (x.id === it.id ? { ...x, sender: e.target.value, sender_type: e.target.value === "老师" ? "teacher" : "parent" } : x)))}>
              <option value="家长">家长</option><option value="老师">老师</option>
            </select>
            <button className="button sm ghost danger" onClick={() => onChange(value.filter((x) => x.id !== it.id))} aria-label="删除作业"><Trash2 size={14} /></button>
          </div>
        ))}
      </div>
      {value.length > 0 ? <p className="tt-hint" style={{ marginTop: 8 }}>已完成 {value.filter((i) => i.done).length} / {value.length} 项 · 勾选后设备显示实心框并划线 · 次日 0 点自动归档</p> : null}
    </div>
  </>;
}

function ReadingEditor({ value, onChange, notify }: { value: ReadingItem[]; onChange: (r: ReadingItem[]) => void; notify: (m: string) => void }) {
  const [title, setTitle] = useState("");
  const [progress, setProgress] = useState("");
  function add() {
    const t = title.trim();
    if (!t) { notify("请填写书名或阅读内容"); return; }
    onChange([...value, { id: Date.now().toString(36) + Math.random().toString(36).slice(2, 6), title: t, progress: progress.trim() }]);
    setTitle("");
    setProgress("");
    notify("阅读任务已添加");
  }
  return <>
    <div className="card-pad">
      <div className="note-compose">
        <div className="field-row">
          <input className="cs-input" value={title} maxLength={40} onChange={(e) => setTitle(e.target.value)} placeholder="书名 / 内容，如：夏洛的网" />
          <input className="cs-input" value={progress} maxLength={20} onChange={(e) => setProgress(e.target.value)} placeholder="进度（选填），如：P32-45" />
        </div>
        <div className="note-compose-foot"><span />
          <button className="button" onClick={add}><Plus size={16} />添加任务</button>
        </div>
      </div>
      <div className="hw-list">
        {value.length === 0 ? <div className="tt-empty">还没有阅读任务，先在上面添加一条。</div> : value.map((it) => (
          <div className="hw-row" key={it.id}>
            <input className="cs-input hw-text" value={it.title} maxLength={40}
              onChange={(e) => onChange(value.map((x) => (x.id === it.id ? { ...x, title: e.target.value } : x)))} />
            <input className="cs-input hw-progress" value={it.progress} maxLength={20} placeholder="进度"
              onChange={(e) => onChange(value.map((x) => (x.id === it.id ? { ...x, progress: e.target.value } : x)))} />
            <button className="button sm ghost danger" onClick={() => onChange(value.filter((x) => x.id !== it.id))} aria-label="删除任务"><Trash2 size={14} /></button>
          </div>
        ))}
      </div>
      <p className="tt-hint" style={{ marginTop: 8 }}>设备上书名会自动加《》显示，进度显示在行右侧。</p>
    </div>
  </>;
}

function HistoryArchive({ content, setContent, notify }: { content: ContentState; setContent: React.Dispatch<React.SetStateAction<ContentState>>; notify: (m: string) => void }) {
  const [q, setQ] = useState("");
  const today = new Date().toISOString().slice(0, 10);
  // 历史 = 非当天的作业 + 非当天且非持续的便签
  const hwHist = content.homework.filter((h) => h.date && h.date < today);
  const noteHist = content.notes.filter((n) => n.date && n.date < today && !n.persistent);
  const rows = [
    ...hwHist.map((h) => ({ type: "作业" as const, id: h.id, text: h.text, sender: h.sender || "家长", sender_type: h.sender_type, date: h.date! })),
    ...noteHist.map((n) => ({ type: "便签" as const, id: n.id, text: n.text, sender: n.sender || "家长", sender_type: n.sender_type, date: n.date! })),
  ].filter((r) => !q || r.text.includes(q) || r.date.includes(q));
  function copyToToday(id: string, type: "作业" | "便签") {
    const t = today;
    const mk = () => Date.now().toString(36) + Math.random().toString(36).slice(2, 6);
    if (type === "作业") {
      const it = content.homework.find((h) => h.id === id);
      if (!it) return;
      setContent({ ...content, homework: [...content.homework, { ...it, id: mk(), date: t, done: false }] });
    } else {
      const it = content.notes.find((n) => n.id === id);
      if (!it) return;
      setContent({ ...content, notes: [...content.notes, { ...it, id: mk(), date: t }] });
    }
    notify("已复制为今日，记得保存并应用到设备");
  }
  function purge(id: string, type: "作业" | "便签") {
    if (type === "作业") setContent({ ...content, homework: content.homework.filter((h) => h.id !== id) });
    else setContent({ ...content, notes: content.notes.filter((n) => n.id !== id) });
  }
  return <>
    <div className="card-pad">
      <p className="tt-hint" style={{ marginBottom: 8 }}>系统每天 0 点自动把「当天」作业 / 单次便签归档于此；可回看、一键复制为今日或清理。</p>
      <input className="cs-input" value={q} onChange={(e) => setQ(e.target.value)} placeholder="搜索历史内容或日期（如 10-16）" />
      <div className="hw-list" style={{ marginTop: 10 }}>
        {rows.length === 0 ? <div className="tt-empty">暂无历史归档（今天之前的作业 / 便签会自动出现在这里）。</div> : rows.map((r) => (
          <div className="hw-row" key={r.type + r.id}>
            <span className="hist-type">{r.type}</span>
            <span className="tx">{r.text}</span>
            <span className="mt">{r.date.slice(5)}{((r.sender_type) || (r.sender === "老师" ? "teacher" : "")) === "teacher" ? " · 师" : ""}</span>
            <button className="button sm" onClick={() => copyToToday(r.id, r.type)} title="复制为今日">复制</button>
            <button className="button sm ghost danger" onClick={() => purge(r.id, r.type)} aria-label="清理"><Trash2 size={14} /></button>
          </div>
        ))}
      </div>
    </div>
  </>;
}

function BillLedgerEditor({ value, onChange, notify }: { value: BillItem[]; onChange: (b: BillItem[]) => void; notify: (m: string) => void }) {
  const today = new Date().toISOString().slice(0, 10);
  const [category, setCategory] = useState<BillCategory>("宽带费");
  const [name, setName] = useState("");
  const [amount, setAmount] = useState("");
  const [cycle, setCycle] = useState<BillCycle>("monthly");
  const [nextDue, setNextDue] = useState(today);
  const [remindDays, setRemindDays] = useState("3");

  function add() {
    const n = name.trim();
    const amt = parseFloat(amount);
    if (!n) { notify("请填写账单名称"); return; }
    if (!(amt >= 0)) { notify("请填写正确的金额"); return; }
    if (!nextDue) { notify("请选择下次缴费日期"); return; }
    const item: BillItem = {
      id: Date.now().toString(36) + Math.random().toString(36).slice(2, 6),
      category, name: n, amount: amt, cycle,
      nextDue, remindDays: Math.max(0, parseInt(remindDays) || 0),
      paid: false, history: [],
    };
    onChange([...value, item]);
    setName(""); setAmount(""); setRemindDays("3");
    notify(`已添加${category}：${n}`);
  }
  function pay(b: BillItem) {
    const history = [...(b.history || []), { date: today, amount: b.amount }];
    const next = advanceDate(b.nextDue, b.cycle);
    onChange(value.map((x) => (x.id === b.id ? { ...x, history, nextDue: next, paid: true } : x)));
    notify(`已核销 ${b.name}（¥${b.amount}），下次${b.cycle === "yearly" ? "年" : "月"}付日期已顺延至 ${next}`);
  }
  function unpay(b: BillItem) {
    onChange(value.map((x) => (x.id === b.id ? { ...x, paid: false } : x)));
    notify(`已撤销 ${b.name} 的已缴标记`);
  }
  const sorted = [...value].sort((a, b) => (a.nextDue < b.nextDue ? -1 : 1));
  const dueSoon = sorted.filter((b) => { const s = billStatus(b, today); return s.key === "soon" || s.key === "overdue"; }).length;
  return <>
    <div className="card-pad">
      <div className="note-compose">
        <div className="field-row">
          <select className="cs-input" value={category} onChange={(e) => setCategory(e.target.value as BillCategory)}>
            {BILL_CATEGORIES.map((c) => <option key={c} value={c}>{c}</option>)}
          </select>
          <input className="cs-input" value={name} maxLength={20} onChange={(e) => setName(e.target.value)} placeholder="名称，如：电信宽带 500M / 通义大模型" />
        </div>
        <div className="field-row">
          <input className="cs-input" type="number" min="0" step="0.01" value={amount} onChange={(e) => setAmount(e.target.value)} placeholder="金额（元）" />
          <select className="cs-input" value={cycle} onChange={(e) => setCycle(e.target.value as BillCycle)}>
            <option value="monthly">月付</option>
            <option value="yearly">年付</option>
          </select>
          <input className="cs-input" type="date" value={nextDue} onChange={(e) => setNextDue(e.target.value)} title="下次应缴日期" />
          <input className="cs-input" type="number" min="0" value={remindDays} onChange={(e) => setRemindDays(e.target.value)} placeholder="提前提醒天数" title="提前提醒天数" style={{ maxWidth: 110 }} />
        </div>
        <div className="note-compose-foot"><span className="tt-hint">支持 月付 / 年付；临近「提前提醒天数」会变黄，逾期标红。</span>
          <button className="button" onClick={add}><Plus size={16} />添加账单</button>
        </div>
      </div>
      {value.length > 0 && (
        <div className="bill-summary">
          <span>共 {value.length} 笔</span>
          <span>临近/逾期 {dueSoon} 笔</span>
          <span>月付 {value.filter((b) => b.cycle === "monthly").length} · 年付 {value.filter((b) => b.cycle === "yearly").length}</span>
        </div>
      )}
      <div className="hw-list">
        {sorted.length === 0 ? <div className="tt-empty">还没有账单，先在上面添加一条（电费、水费、燃气费、伙食费、宽带费、大模型费用等）。</div> : sorted.map((b) => {
          const st = billStatus(b, today);
          const totalPaid = (b.history || []).reduce((s, h) => s + h.amount, 0);
          return (
            <div className="bill-row" key={b.id}>
              <div className="bill-main">
                <span className="bill-cat">{b.category}</span>
                <b className="bill-name">{b.name}</b>
                <span className="bill-amt">¥{b.amount}</span>
                <span className="bill-cycle">{b.cycle === "yearly" ? "年付" : "月付"}</span>
              </div>
              <div className="bill-sub">
                <span>下次：{b.nextDue}</span>
                <span>提醒：提前 {b.remindDays} 天</span>
                <span className={`bill-status ${st.cls}`}>{st.label}</span>
              </div>
              <div className="bill-actions">
                {b.paid && today < b.nextDue
                  ? <button className="button sm ghost" onClick={() => unpay(b)} title="撤销已缴">撤销</button>
                  : <button className="button sm primary" onClick={() => pay(b)}><Check size={14} />一键核销</button>}
                <button className="button sm ghost danger" onClick={() => onChange(value.filter((x) => x.id !== b.id))} aria-label="删除账单"><Trash2 size={14} /></button>
              </div>
              {(b.history && b.history.length > 0) && (
                <details className="bill-history"><summary>缴费历史（{b.history.length} 笔 · 累计 ¥{totalPaid.toFixed(2)}）</summary>
                  <ul>{[...b.history].reverse().map((h, i) => <li key={i}><span>{h.date}</span><b>¥{h.amount}</b></li>)}</ul>
                </details>
              )}
            </div>
          );
        })}
      </div>
      <p className="tt-hint" style={{ marginTop: 8 }}>「一键核销」会记录本次缴费到历史，并按 月付/年付 自动顺延一个周期的下次应缴日期；数据随双屏配置一起下发到设备，便于在墨水屏家庭看板展示。</p>
    </div>
  </>;
}

function FamilyEventsEditor({ title, value, onChange, members = [] }: { title: string; value: FamilyEvent[]; onChange: (items: FamilyEvent[]) => void; members?: FamilyMember[] }) {
  const now = new Date();
  const todayKey = localDateKey(now);
  const [name, setName] = useState("");
  const [date, setDate] = useState(todayKey);
  const [time, setTime] = useState("09:00");
  const [member, setMember] = useState("全家");
  const [category, setCategory] = useState("家庭");
  const [repeat, setRepeat] = useState("不重复");
  const [reminder, setReminder] = useState("提前30分钟");
  const [note, setNote] = useState("");
  const [month, setMonth] = useState(() => new Date(now.getFullYear(), now.getMonth(), 1));
  const [selectedDate, setSelectedDate] = useState(todayKey);
  const memberOptions = Array.from(new Set(["全家", ...members.map(member => member.name).filter(Boolean)]));
  const monthDays = useMemo(() => {
    const first = new Date(month.getFullYear(), month.getMonth(), 1);
    const mondayOffset = (first.getDay() + 6) % 7;
    return Array.from({ length: 42 }, (_, i) => new Date(month.getFullYear(), month.getMonth(), 1 - mondayOffset + i));
  }, [month]);
  const selectedEvents = value.filter(event => event.date === selectedDate).sort((a,b) => (a.time || "99:99").localeCompare(b.time || "99:99"));
  const eventCounts = useMemo(() => value.reduce<Record<string, number>>((map, event) => { map[event.date] = (map[event.date] || 0) + 1; return map; }, {}), [value]);
  const upcomingCount = value.filter(event => event.date >= todayKey).length;
  function moveMonth(offset: number) { setMonth(current => new Date(current.getFullYear(), current.getMonth() + offset, 1)); }
  function addEvent() {
    if (!name.trim()) return;
    const event: FamilyEvent = { id: Date.now().toString(36) + Math.random().toString(36).slice(2, 5), title: name.trim(), date, time, member, category, repeat, reminder, note: note.trim() };
    onChange([...value, event]);
    setName(""); setNote(""); setSelectedDate(date);
  }
  return <div className="schedule-workspace">
    <div className="schedule-overview"><div><span className="kicker">FAMILY CALENDAR</span><h3>{title}</h3><p>统一安排家庭成员的日程、活动和提醒，变更会同步到双屏预览。</p></div><div className="schedule-stats"><span><b>{upcomingCount}</b><small>待进行</small></span><span><b>{new Set(value.map(x => x.date)).size}</b><small>安排日期</small></span><span><b>{members.length || 1}</b><small>家庭成员</small></span></div></div>
    <div className="schedule-workspace-grid">
      <section className="schedule-calendar-card">
        <div className="schedule-month-head"><div><span className="schedule-month-icon"><CalendarDays size={17}/></span><div><b>{month.getFullYear()}年{month.getMonth() + 1}月</b><small>家庭日历 · {value.length} 项安排</small></div></div><div className="schedule-month-actions"><button onClick={() => { setMonth(new Date(now.getFullYear(), now.getMonth(), 1)); setSelectedDate(todayKey); }}>今天</button><button aria-label="上个月" onClick={() => moveMonth(-1)}><ChevronDown size={15} className="month-prev"/></button><button aria-label="下个月" onClick={() => moveMonth(1)}><ChevronDown size={15} className="month-next"/></button></div></div>
        <div className="schedule-calendar-grid"><div className="schedule-weekdays">{["一","二","三","四","五","六","日"].map(day => <span key={day}>{day}</span>)}</div><div className="schedule-days">{monthDays.map(day => { const key = localDateKey(day); const inMonth = day.getMonth() === month.getMonth(); return <button key={key} className={`${inMonth ? "" : "outside"} ${key === selectedDate ? "selected" : ""} ${key === todayKey ? "today" : ""}`} onClick={() => { setSelectedDate(key); setDate(key); }}><span>{day.getDate()}</span>{eventCounts[key] ? <i className={eventCounts[key] > 1 ? "multi" : ""}/> : null}</button>; })}</div></div>
        <div className="schedule-agenda-head"><div><b>{selectedDate === todayKey ? "今天" : `${Number(selectedDate.slice(5,7))}月${Number(selectedDate.slice(8,10))}日`}</b><small>{selectedEvents.length} 项安排</small></div><button className="text-button" onClick={() => { setDate(selectedDate); document.getElementById("family-event-title")?.focus(); }}><Plus size={14}/>添加日程</button></div>
        <div className="schedule-agenda">{selectedEvents.length ? selectedEvents.map(event => <article className="schedule-agenda-event" key={event.id}><time>{event.time || "全天"}</time><i className={`event-marker ${familyEventTone(event.category)}`}/><div><b>{event.title}</b><small>{[event.member || "全家", event.category, event.note].filter(Boolean).join(" · ")}</small></div><button aria-label="删除日程" onClick={() => onChange(value.filter(item => item.id !== event.id))}><Trash2 size={14}/></button></article>) : <div className="schedule-empty"><CalendarDays size={20}/><span>这一天还没有日程</span><small>从右侧快速创建，家庭成员都能看到</small></div>}</div>
      </section>
      <section className="schedule-compose-card">
        <div className="schedule-compose-head"><span><Plus size={17}/></span><div><b>新建日程</b><small>把重要安排分享给家人</small></div></div>
        <label className="schedule-field">标题<input id="family-event-title" value={name} onChange={event => setName(event.target.value)} placeholder="例如：周末去科技馆" /></label>
        <div className="schedule-field"><span>成员</span><div className="schedule-member-pills">{memberOptions.map(option => <button type="button" key={option} className={member === option ? "active" : ""} onClick={() => setMember(option)}>{option}</button>)}</div></div>
        <div className="schedule-fields-two"><label className="schedule-field">日期<input type="date" value={date} onChange={event => { setDate(event.target.value); setSelectedDate(event.target.value); }} /></label><label className="schedule-field">时间<input type="time" value={time} onChange={event => setTime(event.target.value)} /></label></div>
        <div className="schedule-fields-two"><label className="schedule-field">类型<select value={category} onChange={event => setCategory(event.target.value)}>{["家庭","就医","课程","出行","纪念日","其他"].map(item => <option key={item}>{item}</option>)}</select></label><label className="schedule-field">重复<select value={repeat} onChange={event => setRepeat(event.target.value)}>{["不重复","每天","每周","每月","每年"].map(item => <option key={item}>{item}</option>)}</select></label></div>
        <label className="schedule-field">提醒<select value={reminder} onChange={event => setReminder(event.target.value)}>{["不提醒","准时提醒","提前5分钟","提前15分钟","提前30分钟","提前1小时","提前1天"].map(item => <option key={item}>{item}</option>)}</select></label>
        <label className="schedule-field">备注<textarea value={note} onChange={event => setNote(event.target.value)} placeholder="地点、物品或其他提醒" rows={2}/></label>
        <button className="button primary schedule-submit" onClick={addEvent} disabled={!name.trim()}><CalendarDays size={16}/>保存日程</button>
      </section>
    </div>
  </div>;
}
function FamilyListEditor({ title, value, onChange }: { title: string; value: FamilyListItem[]; onChange: (items: FamilyListItem[]) => void }) {
  const [text,setText]=useState(""); return <div className="card-pad"><h3>{title}</h3><div className="note-compose"><div className="field-row"><input className="cs-input" placeholder="采购项目" value={text} onChange={e=>setText(e.target.value)} /><button className="button" onClick={()=>{if(!text.trim())return;onChange([...value,{id:Date.now().toString(36),text:text.trim()}]);setText("")}}><Plus size={16}/>添加</button></div></div><div className="hw-list">{value.length===0?<div className="tt-empty">清单为空</div>:value.map(x=><div className="bill-row" key={x.id}><b>{x.text}</b><button className="button sm ghost danger" onClick={()=>onChange(value.filter(y=>y.id!==x.id))}><Trash2 size={14}/></button></div>)}</div></div>;
}

function ExpressEditor({ value, onChange, notify }: { value: ExpressItem[]; onChange: (e: ExpressItem[]) => void; notify: (m: string) => void }) {
  const today = new Date().toISOString().slice(0, 10);
  const [company, setCompany] = useState("顺丰");
  const [trackingNo, setTrackingNo] = useState("");
  const [pickupCode, setPickupCode] = useState("");
  const [pickupPoint, setPickupPoint] = useState("");
  const [item, setItem] = useState("");
  const [busy, setBusy] = useState("");

  function add() {
    const no = trackingNo.trim();
    const code = pickupCode.trim();
    if (!no && !code) { notify("请填写取件码（运单号为可选）"); return; }
    if (no && value.some((x) => x.trackingNo === no)) { notify("该运单号已存在"); return; }
    const it: ExpressItem = {
      id: Date.now().toString(36) + Math.random().toString(36).slice(2, 6),
      company, trackingNo: no, pickupCode: code, pickupPoint: pickupPoint.trim(), item: item.trim(),
      status: "ordered", trail: [], updatedAt: today,
    };
    onChange([...value, it]);
    setTrackingNo(""); setPickupCode(""); setPickupPoint(""); setItem("");
    notify(code ? `已添加取件码 ${code}` : `已添加${company}快递`);
  }
  async function track(ex: ExpressItem) {
    setBusy(ex.id);
    try {
      const data = await api<{ configured?: boolean; ok?: boolean; state?: number; data?: ExpressTrail[]; message?: string }>(
        `/api/express/track?no=${encodeURIComponent(ex.trackingNo)}&company=${encodeURIComponent(ex.company)}`,
      );
      if (data.configured === false) {
        notify("未配置快递100密钥，已切换为手动维护状态（请在左侧「系统配置」中填写 KUAIDI100_KEY / KUAIDI100_CUSTOMER）");
        return;
      }
      const trail = Array.isArray(data.data) ? data.data : [];
      const map: Record<number, ExpressStatus> = { 0: "ordered", 1: "transit", 2: "transit", 3: "delivering", 4: "signed", 5: "exception" };
      const status = data.state != null && map[data.state] ? map[data.state] : ex.status;
      onChange(value.map((x) => x.id === ex.id ? { ...x, trail, status, updatedAt: today } : x));
      notify(trail.length ? `已更新 ${ex.trackingNo} 的 ${trail.length} 条轨迹` : "暂无轨迹返回");
    } catch (e) {
      notify((e as Error).message);
    } finally { setBusy(""); }
  }
  function setStatus(ex: ExpressItem, s: ExpressStatus) {
    onChange(value.map((x) => x.id === ex.id ? { ...x, status: s, updatedAt: today } : x));
  }
  const inTransit = value.filter((x) => x.status !== "signed" && x.status !== "exception").length;
  return <>
    <div className="card-pad">
      <div className="note-compose">
        <div className="field-row">
          <select className="cs-input" value={company} onChange={(e) => setCompany(e.target.value)}>
            {EXPRESS_COMPANIES.map((c) => <option key={c} value={c}>{c}</option>)}
          </select>
          <input className="cs-input" value={pickupCode} maxLength={24} onChange={(e) => setPickupCode(e.target.value)} placeholder="取件码（主要填写）" />
          <input className="cs-input" value={pickupPoint} maxLength={30} onChange={(e) => setPickupPoint(e.target.value)} placeholder="驿站/柜号（选填）" />
          <input className="cs-input" value={item} maxLength={30} onChange={(e) => setItem(e.target.value)} placeholder="物品（选填）" />
          <input className="cs-input" value={trackingNo} maxLength={40} onChange={(e) => setTrackingNo(e.target.value)} placeholder="运单号（可选，用于物流查询）" />
        </div>
        <div className="note-compose-foot"><span className="tt-hint">家庭看板以取件码/取件点提醒为主；只有填写运单号时才可选用快递100查询。</span>
          <button className="button" onClick={add}><Plus size={16} />添加快递</button>
        </div>
      </div>
      {value.length > 0 && (
        <div className="bill-summary">
          <span>共 {value.length} 件</span>
          <span>在途 {inTransit} 件</span>
          <span>已签收 {value.filter((x) => x.status === "signed").length} 件</span>
        </div>
      )}
      <div className="hw-list">
        {value.length === 0 ? <div className="tt-empty">还没有包裹，添加取件码即可登记。</div> : value.map((ex) => (
          <div className="bill-row" key={ex.id}>
            <div className="bill-main">
              <span className="bill-cat">取件码</span>
              <b className="bill-name">{ex.pickupCode || ex.trackingNo}</b>
              {ex.pickupPoint ? <span>{ex.pickupPoint}</span> : null}
              {ex.item ? <span className="bill-amt">{ex.item}</span> : null}
              <span className={`bill-status ${ex.status === "signed" ? "paid" : ex.status === "exception" ? "overdue" : "soon"}`}>{expressStatusLabel(ex.status)}</span>
            </div>
            <div className="bill-sub">
              <span>更新：{ex.updatedAt}</span>
              <select className="cs-input" style={{ maxWidth: 120 }} value={ex.status} onChange={(e) => setStatus(ex, e.target.value as ExpressStatus)}>
                {EXPRESS_STATUS.map((s) => <option key={s.key} value={s.key}>{s.label}</option>)}
              </select>
              {ex.trackingNo ? <button className="button sm" onClick={() => void track(ex)} disabled={busy === ex.id}>{busy === ex.id ? <LoaderCircle className="spin" size={14} /> : <RefreshCw size={14} />}可选查询物流</button> : null}
            </div>
            {ex.trail.length > 0 && (
              <ul className="express-trail">
                {ex.trail.slice(0, 8).map((t, i) => <li key={i}><span>{t.time}</span><b>{t.context}</b></li>)}
              </ul>
            )}
            <div className="bill-actions">
              <button className="button sm ghost danger" onClick={() => onChange(value.filter((x) => x.id !== ex.id))} aria-label="删除快递"><Trash2 size={14} /></button>
            </div>
          </div>
        ))}
      </div>
      <p className="tt-hint" style={{ marginTop: 8 }}>「查询物流」会调用后端对接的快递100实时接口；若未配置密钥则自动降级为手动维护状态，不影响使用。</p>
    </div>
  </>;
}

function CoursePreview({ ct, colors }: { ct: CourseTable; colors?: number }) {
  const today = (new Date().getDay() + 6) % 7;
  const isColor = (colors || 4) >= 3;
  const dayCourses = (ct.grid[today] || [])
    .map((c, i) => ({ c, i }))
    .filter((x) => x.c && x.c.name)
    .slice(0, 6)
    .map(({ c, i }) => ({ name: c!.name, teacher: c!.teacher || "", room: c!.room, period: i }));
  return (
    <div className={`eink-screen${isColor ? " color" : ""}`}>
      <div className="eink-title"><b>{ct.class_name || "课程表"}</b><span>周{TT_DAYS[today]}{isColor ? " · 彩色" : " · 黑白"}</span></div>
      {dayCourses.length === 0 ? <div className="eink-empty">今日无课</div> : (
        <div className="eink-courses">
          {dayCourses.map((d) => {
            const pd = ct.periods[d.period];
            const t = pd ? `${String(pd.start_hour).padStart(2, "0")}:${String(pd.start_minute).padStart(2, "0")}` : "";
            return <div key={d.period}><span className="ck">{d.name}{d.teacher ? `（${d.teacher}）` : ""}</span><span className="ct">{t}</span><b>{d.room}</b></div>;
          })}
        </div>
      )}
      <div className="eink-foot"><span>{ct.show_days === 7 ? "全周" : "工作日"}</span><span>{ct.school || "课程表"}</span></div>
    </div>
  );
}

function NotesPreview({ notes }: { notes: Note[] }) {
  return (
    <div className="eink-screen">
      <div className="eink-title"><b>便签</b><span>共 {notes.length} 条</span></div>
      {notes.length === 0 ? <div className="eink-empty">暂无便签</div> : (
        <div className="eink-rows">
          {notes.slice(0, 6).map((n) => {
            const col = TT_COLORS.find((c) => c.key === n.color) || TT_COLORS[0];
            const st = n.sender_type || (n.sender === "老师" ? "teacher" : (n.sender === "家长" ? "parent" : ""));
            const prefix = st === "teacher" ? "师·" : "";
            return <div key={n.id}><i className="eink-tick" style={{ background: col.bd }} /><span className="tx">{prefix}{n.text}</span></div>;
          })}
        </div>
      )}
    </div>
  );
}

function HomeworkPreview({ items }: { items: HomeworkItem[] }) {
  const done = items.filter((i) => i.done).length;
  return (
    <div className="eink-screen">
      <div className="eink-title"><b>今日作业</b><span>共 {items.length} 项</span></div>
      {items.length === 0 ? <div className="eink-empty">暂无作业</div> : (
        <div className="eink-rows">
          {items.slice(0, 6).map((it) => (
            <div key={it.id} className={it.done ? "done" : ""}>
              <i className={`eink-check${it.done ? " on" : ""}`} />
              <span className="tx">{((it.sender_type || (it.sender === "老师" ? "teacher" : "")) === "teacher" ? "师·" : "")}{it.text}</span>
            </div>
          ))}
        </div>
      )}
      {items.length > 0 ? <div className="eink-sub">已完成 {done} / {items.length}</div> : null}
    </div>
  );
}

function ReadingPreview({ items }: { items: ReadingItem[] }) {
  return (
    <div className="eink-screen">
      <div className="eink-title"><b>阅读任务</b><span>共 {items.length} 项</span></div>
      {items.length === 0 ? <div className="eink-empty">暂无阅读任务</div> : (
        <div className="eink-rows">
          {items.slice(0, 6).map((it) => (
            <div key={it.id}>
              <span className="tx">{it.title.startsWith("《") ? it.title : `《${it.title}》`}</span>
              {it.progress ? <span className="mt">{it.progress}</span> : null}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}

function AlbumEditor({ value, onChange, notify }: { value: AlbumState; onChange: (a: AlbumState) => void; notify: (m: string) => void }) {
  const fileRef = useRef<HTMLInputElement>(null);
  const [busy, setBusy] = useState(false);
  async function uploadOne(file: File) {
    const fd = new FormData();
    fd.append("file", file);
    const tok = getToken();
    const res = await fetch("/backend/api/uploads", { method: "POST", headers: tok ? { Authorization: `Bearer ${tok}` } : {}, body: fd });
    if (!res.ok) throw new Error(`上传失败 ${res.status}`);
    const j = await res.json();
    return { id: j.id as string, url: j.url as string, caption: "" };
  }
  async function onPick(e: React.ChangeEvent<HTMLInputElement>) {
    const files = Array.from(e.target.files || []);
    if (!files.length) return;
    setBusy(true);
    try {
      const added: AlbumPhoto[] = [];
      for (const f of files) added.push(await uploadOne(f));
      onChange({ ...value, photos: [...value.photos, ...added] });
      notify(`已添加 ${added.length} 张照片`);
    } catch (err) { notify((err as Error).message); }
    finally { setBusy(false); if (fileRef.current) fileRef.current.value = ""; }
  }
  function remove(id: string) { onChange({ ...value, photos: value.photos.filter((p) => p.id !== id) }); }
  function setCaption(id: string, caption: string) {
    onChange({ ...value, photos: value.photos.map((p) => (p.id === id ? { ...p, caption } : p)) });
  }
  return (
    <div className="member-card">
      <h3>相册</h3>
      <p className="muted">上传家庭照片，墨水屏在「无课 / 便签为空」等无内容时自动展示相册，周末无课程表也会切换为相册。照片按「日」轮换展示。</p>
      <input ref={fileRef} type="file" accept="image/*" multiple hidden onChange={onPick} />
      <div className="row" style={{ alignItems: "center" }}>
        <button className="button" onClick={() => fileRef.current?.click()} disabled={busy}>{busy ? <LoaderCircle className="spin" size={16} /> : <ImageIcon size={16} />}上传照片</button>
        <span className="muted">共 {value.photos.length} 张</span>
      </div>
      {value.photos.length === 0 ? <div className="eink-empty">暂无照片，先上传几张吧</div> : (
        <div className="album-grid">
          {value.photos.map((p) => (
            <div key={p.id} className="album-item">
              <img src={p.url} alt={p.caption || "相册照片"} />
              <input className="cs-input" placeholder="说明文字（可选）" value={p.caption} onChange={(e) => setCaption(p.id, e.target.value)} />
              <button className="button soft danger" onClick={() => remove(p.id)}><Trash2 size={14} />删除</button>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}

function AlbumPreview({ album }: { album: AlbumState }) {
  const photos = album.photos || [];
  const first = photos[0];
  return (
    <div className="eink-screen album-screen">
      <div className="eink-title"><b>相册</b><span>共 {photos.length} 张</span></div>
      {!first ? <div className="eink-empty">暂无照片</div> : (
        <div className="album-prev">
          <img src={first.url} alt={first.caption || "相册照片"} />
        </div>
      )}
    </div>
  );
}

function aModeLabel(mode?: string): string {
  return A_MODES.find((m) => m.key === (mode || "notes"))?.label || "便签";
}

function aModePreview(mode: string, content: ContentState): React.ReactNode {
  if (mode === "homework") return <HomeworkPreview items={content.homework} />;
  if (mode === "reading") return <ReadingPreview items={content.reading} />;
  if (mode === "album") return <AlbumPreview album={content.album} />;
  return <NotesPreview notes={content.notes} />;
}

// 双屏实时预览：A 屏=便签/作业/阅读（黑白），B 屏=课程表（彩色），编辑时联动刷新
function DualPreview({ content, screen, sceneKey, aMode, bMode, weatherPreview, weatherLoading, weatherError }: { content: ContentState; screen: { size: "4.2" | "5.83"; colors: 2 | 3 | 4 }; sceneKey: SceneKey; aMode?: string; bMode?: string; weatherPreview?: DualWeatherPreview | null; weatherLoading?: boolean; weatherError?: string }) {
  const dims = screen.size === "5.83" ? "648 × 480" : "400 × 300";
  const bColor = screen.colors === 2 ? "黑白" : screen.colors === 3 ? "黑白红" : "黑白红黄";
  const mode = aMode || "notes";
  const family = sceneKey === "family";
  return (
    <div className="card-pad dual-preview-card">
      <div className="tt-grid-head"><span>双屏实时预览</span><span className="tt-hint">{family ? `A 屏${aMode === "family-weather" ? "天气" : aMode === "family" ? "家庭共享看板" : "家庭提醒"} · B 屏${bMode === "calendar" ? "家庭日程" : bMode === "family" ? "家庭共享看板" : "相册"}` : `A 屏${aModeLabel(mode)}（黑白快刷） · B 屏课程表（${bColor}）`}，编辑即时刷新</span></div>
      <div className="dual-preview">
        <div className="preview-card a">
          <div className="preview-meta"><span>实时预览 · A 屏</span><small>{dims}</small></div>
          {family ? aMode === "family-weather"
            ? <WeatherBoard data={weatherPreview} loading={!!weatherLoading} error={weatherError || ""} city={content.weatherCity || "设备城市"} />
            : <FamilyTodayScreen content={content} mode={aMode} />
            : aModePreview(mode, content)}
        </div>
        <div className="dp-mid"><span>双屏协同</span></div>
        <div className="preview-card b">
          <div className="preview-meta"><span>实时预览 · B 屏</span><small>{dims} · {bColor}</small></div>
          {family ? bMode === "calendar" ? <FamilyWeekScreen content={content} /> : bMode === "album" ? <AlbumPreview album={content.album} /> : <FamilyTodayScreen content={content} mode="family" /> : (() => {
            const todayIdx = (new Date().getDay() + 6) % 7;
            const hasCourse = (content.courseTable.periods || []).some((_, p) => !!content.courseTable.grid?.[todayIdx]?.[p]?.name);
            return !hasCourse && (content.album?.photos?.length || 0) > 0
              ? <AlbumPreview album={content.album} />
              : <CoursePreview ct={content.courseTable} colors={screen.colors} />;
          })()}
        </div>
      </div>
      <div className="dp-hardware">
        <span><i className="dot-a" />A 屏 · FPC1 · 黑白快刷 SSD1619（{dims}）</span>
        <span><i className="dot-b" />B 屏 · FPC2 · SSD2683ZA（{dims} · {bColor}）</span>
      </div>
    </div>
  );
}

function localDateKey(date: Date) { return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, "0")}-${String(date.getDate()).padStart(2, "0")}`; }
function familyEventTone(category?: string) { if (/就医|医疗/.test(category || "")) return "coral"; if (/课程|学习/.test(category || "")) return "teal"; if (/出行/.test(category || "")) return "gold"; return "green"; }
function FamilyTodayScreen({ content, mode = "family" }: { content: ContentState; mode?: string }) {
  const today = localDateKey(new Date());
  const events = content.familySchedule.filter(x => x.date === today).sort((a,b) => (a.time || "99:99").localeCompare(b.time || "99:99"));
  const visibleEvents = events.length ? events.slice(0, 4) : content.familySchedule.filter(x => x.date >= today).sort((a,b) => `${a.date}${a.time || ""}`.localeCompare(`${b.date}${b.time || ""}`)).slice(0, 4);
  const todos = content.homework.filter(x => !x.done).slice(0, 3);
  // Older/local content may omit optional fields despite the current Note type.
  const note = (Array.isArray(content.notes) ? content.notes : []).slice().sort((a,b) => String(b?.date || "").localeCompare(String(a?.date || "")))[0];
  const eventsToShow = mode === "family-todos" ? [] : visibleEvents;
  return <div className="eink-screen family-day-screen">
    <header className="family-screen-head"><div><b>{new Date().toLocaleDateString("zh-CN", { month: "long", day: "numeric" })}</b><span>{new Date().toLocaleDateString("zh-CN", { weekday: "long" })}</span></div><small>家庭今日总览</small></header>
    <div className="family-day-grid">
      <section className="family-timeline"><div className="family-section-title"><b>{mode === "family-todos" ? "待办与便签" : events.length ? `今天，全家 ${events.length} 项安排` : "近期家庭安排"}</b><span>{mode === "family-todos" ? `${todos.length} 项待办` : events.length ? "今日" : "接下来"}</span></div>
        {eventsToShow.length ? eventsToShow.map((event) => <article className="family-event-row" key={event.id}><time>{event.date === today ? (event.time || "全天") : `${event.date.slice(5)} ${event.time || ""}`}</time><i className={`event-marker ${familyEventTone(event.category)}`}/><div><b>{event.title}</b><small>{[event.member || "全家", event.note].filter(Boolean).join(" · ")}</small></div>{event.category && <em>{event.category}</em>}</article>) : mode === "family-todos" && todos.length ? todos.map(todo => <article className="family-event-row" key={todo.id}><time>待办</time><i className="event-marker teal"/><div><b>{todo.text}</b><small>家庭任务</small></div></article>) : <div className="family-preview-empty">今天还没有安排</div>}
      </section>
      <aside className="family-side-widgets"><section className="family-mini-widget"><div className="family-section-title"><b>今日待办</b><span>{todos.length} 项</span></div>{todos.length ? todos.map(todo => <div className="family-check-row" key={todo.id}><i/>{todo.text}</div>) : <div className="family-widget-empty">暂无待办</div>}</section><section className="family-mini-widget family-note-widget"><div className="family-section-title"><b>最新留言</b><span>{note ? note.sender : "家庭"}</span></div><p>{note?.text || "家人留言会显示在这里"}</p></section></aside>
    </div>
    <footer className="family-screen-footer"><span>家庭日程</span><span>{content.familyMembers.length || 1} 位成员 · 已同步</span></footer>
  </div>;
}
function FamilyWeekScreen({ content }: { content: ContentState }) {
  const today = new Date();
  const monday = new Date(today); const offset = (today.getDay() + 6) % 7; monday.setDate(today.getDate() - offset);
  const days = Array.from({ length: 7 }, (_, i) => { const d = new Date(monday); d.setDate(monday.getDate() + i); return d; });
  const events = content.familySchedule.filter(event => event.date >= localDateKey(monday) && event.date <= localDateKey(new Date(monday.getFullYear(), monday.getMonth(), monday.getDate() + 6))).sort((a,b) => `${a.date}${a.time || ""}`.localeCompare(`${b.date}${b.time || ""}`));
  const weekEnd = days[6];
  return <div className="eink-screen family-week-screen">
    <header className="family-week-head"><div><b>本周安排</b><span>{days[0].toLocaleDateString("zh-CN", { month: "numeric", day: "numeric" })} — {weekEnd.toLocaleDateString("zh-CN", { month: "numeric", day: "numeric" })}</span></div><small>家庭周历</small></header>
    <div className="family-week-grid"><div className="family-week-days">{days.map(day => <div className={localDateKey(day) === localDateKey(today) ? "today" : ""} key={localDateKey(day)}><span>{["一","二","三","四","五","六","日"][((day.getDay()+6)%7)]}</span><b>{day.getDate()}</b></div>)}</div><div className="family-week-columns">{days.map(day => { const dateKey = localDateKey(day); const dayEvents = events.filter(event => event.date === dateKey).slice(0, 3); return <div className={`family-week-column ${dateKey === localDateKey(today) ? "today" : ""}`} key={dateKey}>{dayEvents.length ? dayEvents.map(event => <div className={`family-week-event ${familyEventTone(event.category)}`} key={event.id}><b>{event.title}</b><small>{event.time || "全天"}{event.member ? ` · ${event.member}` : ""}</small></div>) : <span className="family-week-empty"/>}</div>; })}</div></div>
    <footer className="family-screen-footer"><span>本周 {events.filter(x => x.date >= localDateKey(days[0]) && x.date <= localDateKey(weekEnd)).length} 项安排</span><span>{content.familyMembers.length || 1} 位家庭成员</span></footer>
  </div>;
}

function Modal({title,subtitle,onClose,children}:{title:string;subtitle:string;onClose:()=>void;children:React.ReactNode}){return <div className="modal-backdrop" onMouseDown={e=>e.currentTarget===e.target&&onClose()}><div className="modal"><button className="modal-close" onClick={onClose}><X size={18}/></button><div className="modal-brand">悟</div><h2>{title}</h2><p>{subtitle}</p><div className="modal-body">{children}</div></div></div>}
