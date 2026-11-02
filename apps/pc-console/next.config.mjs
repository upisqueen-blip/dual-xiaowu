// 后端统一为 xiaowu_platform（8104）。旧兼容后端 services/legacy-api 已让出 8104，
// 默认地址同步更新；如需指向别处，用 XIAOWU_API_BASE 覆盖。
const backend = (process.env.XIAOWU_API_BASE || "http://127.0.0.1:8104").replace(/\/$/, "");
const config = {
  // 子路径部署：线上挂在 dual.xiaowuai.vip/developer 下。
  // 加了 basePath 后，/_next 静态资源、/public 资源、以及路由都会自动带 /developer 前缀，
  // 避免白屏 404。务必让反向代理不要 strip /developer 前缀（proxy_pass 末尾不要加 /）。
  basePath: "/developer",
  async rewrites() {
    return [{ source: "/backend/:path*", destination: `${backend}/:path*` }];
  }
};
export default config;
