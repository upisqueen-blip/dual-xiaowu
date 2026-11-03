import type { NextConfig } from "next";

// Next 16 会在执行本文件之前就读完本目录的 .env（构建日志里的 Environments 行），
// 所以这里可以直接用 process.env，无需再手动 loadEnvConfig。
// 加载顺序：.env.local > .env.[NODE_ENV] > .env

// 后端地址只从 .env 读（INKSIGHT_BACKEND_API_BASE），代码里不写死端口。
// 换后端 / 换机器只改 .env，不用动代码。
const backend = (process.env.INKSIGHT_BACKEND_API_BASE || "").trim().replace(/\/$/, "");

if (!backend) {
  console.warn(
    [
      "",
      "[content-console-cn] .env 缺少 INKSIGHT_BACKEND_API_BASE，/api 将不可用。",
      "  处理：复制 apps/content-console-cn/.env.example 为 apps/content-console-cn/.env 并填写。",
      "",
    ].join("\n"),
  );
}

// 路由前缀由 .env 的 NEXT_PUBLIC_BASE_PATH 决定，同一份代码可挂在两种位置：
//   ""        → 独立二级域名根路径（https://dualcn.xiaowuai.vip/，默认值）
//   "/admin"  → 门户反代子路径（https://dual.xiaowuai.vip/admin，需显式配置）
// basePath 会统一作用于路由、/_next 静态资源、<Link>；但 <img src> 这类原生标签
// 不会被自动加前缀，页面内走 lib/api 的 BASE_PATH 手动拼接。
// 默认根路径部署：内容后台是独立域名，接口就是 /api/*，不再带 /admin 前缀。
const basePath = (process.env.NEXT_PUBLIC_BASE_PATH ?? "").trim().replace(/\/$/, "");

const nextConfig: NextConfig = {
  basePath,
  // 构建产物目录。默认 .next；本地已有 dev/start 实例占用 .next（Windows 会锁文件、
  // 构建无法清理）时，可 NEXT_DIST_DIR=.next-verify 另建一份做并行校验。
  distDir: process.env.NEXT_DIST_DIR || ".next",
  // Next.js 15+ 默认拦截"跨域"dev 资源（含 HMR WebSocket）。用 127.0.0.1 访问时
  // 会被判定为跨域，导致 /_next/hmr 握手被拒、页面 hydration 永远挂起（一直转圈）。
  // 允许的 dev 来源（仅开发期跨域/HMR 校验用），可用 .env 的 ALLOWED_DEV_ORIGINS 覆盖（逗号分隔）。
  allowedDevOrigins: (process.env.ALLOWED_DEV_ORIGINS || "localhost,127.0.0.1,192.168.1.4,192.168.1.5,192.168.1.69,192.168.101.69")
    .split(",")
    .map((s) => s.trim())
    .filter(Boolean),
  experimental: {
    // Windows can lock the same prerender segment directory when many build
    // workers create it at once. One worker keeps local/release builds stable.
    cpus: 1,
  },
  async rewrites() {
    if (!backend) return [];
    return [{ source: "/api/:path*", destination: `${backend}/api/:path*` }];
  },
  // basePath 非空时，直接访问本实例根路径 / 会 404（Next 不会自动补前缀）。
  // 这里加一条「不受 basePath 影响」的跳转：/ -> /admin，
  // 避免本地直接开 :8094 看到空白 404，误以为服务没起来 / 门户丢了。
  // basePath 为空（独立域名根路径部署）时不能加，否则 / -> / 死循环。
  async redirects() {
    if (!basePath) return [];
    return [{ source: "/", destination: basePath, permanent: false, basePath: false }];
  },
  // HTML 每次回源校验，避免构建后旧 HTML 引用已删除的 chunk（表现「布局全乱」）。
  async headers() {
    return [
      { source: "/", headers: [{ key: "Cache-Control", value: "no-store, must-revalidate" }] },
    ];
  },
};

export default nextConfig;
