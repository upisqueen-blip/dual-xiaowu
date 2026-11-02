// Next 16 会在执行本文件之前就读完本目录的 .env（构建日志里的 Environments 行），
// 所以这里可以直接用 process.env，不用手动 loadEnvConfig。
// 加载顺序：.env.local > .env.[NODE_ENV] > .env

// 上游地址一律来自 .env（apps/pc-console/.env），代码里不写死任何端口 / 主机：
//   XIAOWU_API_BASE            平台后端，/backend/* 反代到这里
//   XIAOWU_CONTENT_CONSOLE_URL 内容配置台，/admin/* 反代到这里（填根地址，不要带 /admin）
// 改动地址只需改 .env，不用动代码、不用改 Nginx。
const upstream = (name) => (process.env[name] || "").trim().replace(/\/$/, "");

const backend = upstream("XIAOWU_API_BASE");
const contentConsole = upstream("XIAOWU_CONTENT_CONSOLE_URL");

// 缺配置时明确报出来，避免「以为配好了，结果 404/502」排查半天。
const missing = [
  ["XIAOWU_API_BASE", backend, "/backend 接口不可用"],
  ["XIAOWU_CONTENT_CONSOLE_URL", contentConsole, "/admin 内容配置台不可用"],
].filter(([, value]) => !value);

if (missing.length) {
  console.warn(
    [
      "",
      "[pc-console] .env 缺少以下配置：",
      ...missing.map(([key, , effect]) => `  - ${key}  （${effect}）`),
      "  处理：复制 apps/pc-console/.env.example 为 apps/pc-console/.env 并填写。",
      "",
    ].join("\n"),
  );
}

// HTML 文档必须每次回源校验：Next 的 chunk 文件名每次构建都会变，旧文件会被删除，
// 一旦 HTML 被 CDN / Nginx 长缓存，旧 HTML 就会引用 404 的 CSS，表现为「布局全乱」。
// 静态资源（/_next/static）带内容哈希，不在此处禁用缓存，可正常长缓存。
const noStore = [{ key: "Cache-Control", value: "no-store, must-revalidate" }];

const config = {
  // 站点根路径即「创作者门户」：/  -> 门户(截图+登录)
  // /center -> 运营总览（设备、内容任务、审计）
  // /admin  -> 内容配置后台（反代到 .env 里配置的 XIAOWU_CONTENT_CONSOLE_URL）
  async rewrites() {
    return [
      backend && { source: "/backend/:path*", destination: `${backend}/:path*` },
      contentConsole && { source: "/admin/:path*", destination: `${contentConsole}/admin/:path*` },
    ].filter(Boolean);
  },
  async redirects() {
    return [
      // 旧的 /developer 入口已并入根路径，做 301 兼容旧书签
      { source: "/developer", destination: "/", permanent: false },
      { source: "/developer/:path*", destination: "/", permanent: false },
      // 内容配置说明页已由真正的 /admin 后台取代
      { source: "/content-config", destination: "/admin", permanent: false },
    ];
  },
  async headers() {
    return [
      { source: "/", headers: noStore },
      { source: "/center", headers: noStore },
      { source: "/api-docs", headers: noStore },
    ];
  },
};

export default config;
