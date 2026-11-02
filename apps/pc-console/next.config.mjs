// 先加载本目录的 .env：Next 读取 next.config.mjs 时 .env 尚未加载，
// 不手动加载的话 XIAOWU_API_BASE 永远取不到值，会退回默认地址。
import nextEnv from "@next/env";
import { fileURLToPath } from "node:url";

nextEnv.loadEnvConfig(fileURLToPath(new URL(".", import.meta.url)));

// 后端统一为 xiaowu_platform（8104）。
// 内容配置台是独立应用 content-console-cn（8094），在 /admin 子路径下对外。
const backend = (process.env.XIAOWU_API_BASE || "http://127.0.0.1:8104").replace(/\/$/, "");
const contentConsole = (process.env.XIAOWU_CONTENT_CONSOLE_URL || "http://127.0.0.1:8094").replace(/\/$/, "");

// HTML 文档必须每次回源校验：Next 的 chunk 文件名每次构建都会变，旧文件会被删除，
// 一旦 HTML 被 CDN / Nginx 长缓存，旧 HTML 就会引用 404 的 CSS，表现为「布局全乱」。
// 静态资源（/_next/static）带内容哈希，不在此处禁用缓存，可正常长缓存。
const noStore = [{ key: "Cache-Control", value: "no-store, must-revalidate" }];

const config = {
  // 站点根路径即「创作者门户」：/  -> 门户(截图+登录)
  // /center -> 运营总览（设备、内容任务、审计）
  // /admin  -> 内容配置后台（反代到 content-console-cn:8094，其自身 basePath=/admin）
  async rewrites() {
    return [
      { source: "/backend/:path*", destination: `${backend}/:path*` },
      { source: "/admin/:path*", destination: `${contentConsole}/admin/:path*` },
    ];
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
