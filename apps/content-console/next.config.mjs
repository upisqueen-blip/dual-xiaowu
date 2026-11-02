// 后端地址只从 .env 读（XIAOWU_API_BASE），源码里不写死端口 / 主机。
// 缺失时不注册 rewrite（避免静默转发到错误端口），并给出明确告警。
const backend = (process.env.XIAOWU_API_BASE || "").trim().replace(/\/$/, "");

if (!backend) {
  console.warn(
    "\n[content-console] .env 缺少 XIAOWU_API_BASE，/backend 反代不可用。\n" +
      "  处理：复制 apps/content-console/.env.example 为 apps/content-console/.env 并填写。\n",
  );
}

const config = {
  async rewrites() {
    if (!backend) return [];
    return [{ source: "/backend/:path*", destination: `${backend}/:path*` }];
  },
};
export default config;
