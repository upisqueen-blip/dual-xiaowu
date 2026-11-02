import type { NextConfig } from "next";

const nextConfig: NextConfig = {
  // 对外挂在 dual.xiaowuai.vip/admin 下；basePath 让路由、/_next 资源、Link 统一带 /admin 前缀。
  // 注意：<img src="/..."> 这类原生标签不会被自动加前缀，已在页面内手动写成 /admin/...。
  basePath: "/admin",
  // Next.js 15+ 默认拦截"跨域"dev 资源（含 HMR WebSocket）。用 127.0.0.1 访问时
  // 会被判定为跨域，导致 /_next/hmr 握手被拒、页面 hydration 永远挂起（一直转圈）。
  allowedDevOrigins: ["localhost", "127.0.0.1", "192.168.1.4", "192.168.1.5", "192.168.1.69", "192.168.101.69"],
  experimental: {
    // Windows can lock the same prerender segment directory when many build
    // workers create it at once. One worker keeps local/release builds stable.
    cpus: 1,
  },
  async rewrites() {
    const backend = process.env.INKSIGHT_BACKEND_API_BASE?.replace(/\/$/, "") || "http://127.0.0.1:8104";
    return [{ source: "/api/:path*", destination: `${backend}/api/:path*` }];
  },
  // HTML 每次回源校验，避免构建后旧 HTML 引用已删除的 chunk（表现「布局全乱」）。
  async headers() {
    return [
      { source: "/", headers: [{ key: "Cache-Control", value: "no-store, must-revalidate" }] },
    ];
  },
};

export default nextConfig;
