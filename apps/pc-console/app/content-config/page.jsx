"use client";

// 说明页：内容配置后台（用户侧）是独立的前端应用（apps/content-console），
// 与运营后台（本应用）分属两个受众：本应用给管理员用，内容后台给买设备的人用。
// 默认开发地址为 http://localhost:8094（start_all.bat / start_console.bat 启动的 content-console-cn），
// 可在下方常量中调整或改成线上域名；海外部署请设置 NEXT_PUBLIC_CONTENT_CONSOLE_URL 覆盖。
const CONTENT_CONSOLE_URL = process.env.NEXT_PUBLIC_CONTENT_CONSOLE_URL || "http://localhost:8094";

export default function ContentConfigLanding() {
  return (
    <main className="page">
      <section className="landing">
        <span className="kicker">DUAL CONTENT CONSOLE</span>
        <h1>双屏内容配置后台</h1>
        <p>
          这是给<strong>购买设备的人</strong>使用的用户后台：登录账号、绑定设备、配置场景与双屏、
          编辑课程表 / 便签 / 作业 / 阅读 / 相册 / 账单 / 快递等，并邀请家庭成员、给老师生成授权码。
        </p>
        <p className="muted">运营后台（你当前所在）只给管理员使用，不包含上述用户功能。</p>
        <a className="button primary" href={CONTENT_CONSOLE_URL} target="_blank" rel="noreferrer">
          打开内容配置后台 →
        </a>
        <p className="hint">若地址不正确，请设置环境变量 <code>NEXT_PUBLIC_CONTENT_CONSOLE_URL</code> 后重启本应用。</p>
      </section>
    </main>
  );
}
