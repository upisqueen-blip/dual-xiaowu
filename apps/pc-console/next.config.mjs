// Next 16 会在执行本文件之前就读完本目录的 .env（构建日志里的 Environments 行），
// 所以这里可以直接用 process.env，不用手动 loadEnvConfig。
// 加载顺序：.env.local > .env.[NODE_ENV] > .env

// 上游地址一律来自 .env（apps/pc-console/.env），代码里不写死任何端口 / 主机：
//   XIAOWU_API_BASE            平台后端，/backend/* 反代到这里
//   XIAOWU_CONTENT_CONSOLE_URL 内容配置台，/admin/* 反代到这里（填根地址，不要带 /admin）
// 改动地址只需改 .env，不用动代码、不用改 Nginx。
const upstream = (name) => (process.env[name] || "").trim().replace(/\/$/, "");

const backend = upstream("XIAOWU_API_BASE");
// 约定填「根地址」（不要带 /admin）。但线上很容易顺手填成 http://host:8094/admin，
// 那样会拼成 /admin/admin/... 直接 404，排查起来极难。这里自动剥掉尾部 /admin。
const contentConsole = upstream("XIAOWU_CONTENT_CONSOLE_URL").replace(/\/admin$/, "");

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
  // 前端为客户端组件，跳转「双屏内容配置」时直接用 XIAOWU_CONTENT_CONSOLE_URL 配置的
  // 根地址本身（如 https://dualcn.xiaowuai.vip），而不是相对路径 /admin。
  // 通过 env 字段把已 trim、剥除尾部 /admin 的 contentConsole 注入 process.env，构建时内联。
  env: {
    NEXT_PUBLIC_CONTENT_CONSOLE_URL: contentConsole,
  },
  // Next 15+ 在 dev 下会校验 /_next 资源请求的来源域名。本地改用域名访问
  // （dual.localhost，经 dev-proxy 转发）时，Host 不再是 localhost / IP，
  // 不放行就会被判定跨域，表现为 HMR 握手失败、页面一直转圈。
  // 线上走标准域名访问，不受此项影响。
  allowedDevOrigins: (
    process.env.ALLOWED_DEV_ORIGINS ||
    "localhost,127.0.0.1,dual.localhost,dualcn.localhost"
  )
    .split(",")
    .map((s) => s.trim())
    .filter(Boolean),
  async rewrites() {
    return [
      backend && { source: "/backend/:path*", destination: `${backend}/:path*` },
      // 裸 /admin 单独一条：不能指望 "/admin/:path*" 一定匹配零段
      // （不同 Next 版本 path-to-regexp 行为不一致，缺了就是「/admin 404、/admin/pair 正常」）
      contentConsole && { source: "/admin", destination: `${contentConsole}/admin` },
      contentConsole && { source: "/admin/:path*", destination: `${contentConsole}/admin/:path*` },
    ].filter(Boolean);
  },
  async redirects() {
    return [
      // /developer 已是真实子入口（app/developer），不再重定向到 /
      // 内容配置说明页已由真正的 /admin 后台取代
      { source: "/content-config", destination: "/admin", permanent: false },
    ];
  },
  async headers() {
    return [
      { source: "/", headers: noStore },
      { source: "/center", headers: noStore },
      { source: "/api-docs", headers: noStore },
      { source: "/try", headers: noStore },
    ];
  },
};

export default config;
