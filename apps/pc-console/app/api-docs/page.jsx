import Link from "next/link";
import ApiDocs from "./ApiDocs";

export const metadata = {
  title: "小悟 ISV API 文档",
  description: "第三方应用接入小悟双屏的接口规范：在线查看或下载 OpenAPI",
};

// 服务端直出：规范内容不依赖浏览器 JS，禁用了脚本也能读到文档。
const API_ORIGIN = (process.env.XIAOWU_API_BASE || "http://127.0.0.1:8104").replace(/\/$/, "");

async function loadSpec() {
  try {
    const [specRes, mdRes] = await Promise.all([
      fetch(`${API_ORIGIN}/openapi/isv.json`, { cache: "no-store" }),
      fetch(`${API_ORIGIN}/openapi/isv.md`, { cache: "no-store" }),
    ]);
    if (!specRes.ok) return { spec: null, md: "" };
    return { spec: await specRes.json(), md: mdRes.ok ? await mdRes.text() : "" };
  } catch {
    return { spec: null, md: "" };
  }
}

export default async function ApiDocsPage() {
  const { spec, md } = await loadSpec();
  return (
    <div className="api-shell">
      <header className="api-top">
        <Link href="/developer" className="api-brand">
          <b>小悟众创</b>
          <span>ISV API DOCS</span>
        </Link>
        <nav>
          <Link href="/developer">开发者中心</Link>
          <a href="/developer/backend/openapi/isv.json?download=1">下载规范</a>
        </nav>
      </header>
      <ApiDocs initialSpec={spec} initialMd={md} />
    </div>
  );
}
