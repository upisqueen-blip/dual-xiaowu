// 先加载本目录的 .env：Next 读取 next.config.mjs 时 .env 尚未加载，
// 不手动加载的话 XIAOWU_API_BASE 永远取不到值，会退回默认地址。
import nextEnv from "@next/env";
import { fileURLToPath } from "node:url";

nextEnv.loadEnvConfig(fileURLToPath(new URL(".", import.meta.url)));

// 后端统一为 xiaowu_platform（8104）。旧兼容后端 services/legacy-api 已让出 8104，
// 默认地址同步更新；如需指向别处，用 XIAOWU_API_BASE 覆盖（.env 或真实环境变量均可）。
const backend = (process.env.XIAOWU_API_BASE || "http://127.0.0.1:8104").replace(/\/$/, "");

// HTML 文档必须每次回源校验：Next 的 chunk 文件名每次构建都会变，旧文件会被删除，
// 一旦 HTML 被 CDN / Nginx 长缓存，旧 HTML 就会引用 404 的 CSS，表现为「布局全乱」。
// 静态资源（/_next/static）带内容哈希，不在此处禁用缓存，可正常长缓存。
// 注意：配置了 basePath 后，headers 的 source 会自动带上 /developer 前缀，
// 所以这里写 "/" 实际命中 /developer/ 首页，无需再写 "/developer"（会拼成无效路由）。
const noStore = [{ key: "Cache-Control", value: "no-store, must-revalidate" }];

const config = {
  // 子路径部署：线上挂在 dual.xiaowuai.vip/developer 下。
  // 加了 basePath 后，/_next 静态资源、/public 资源、以及路由都会自动带 /developer 前缀，
  // 避免白屏 404。务必让反向代理不要 strip /developer 前缀（proxy_pass 末尾不要加 /）。
  basePath: "/developer",
  async rewrites() {
    return [{ source: "/backend/:path*", destination: `${backend}/:path*` }];
  },
  async headers() {
    return [
      { source: "/", headers: noStore },
      { source: "/content-config", headers: noStore },
    ];
  },
};

export default config;
