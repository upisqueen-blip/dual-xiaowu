export const TOKEN_KEY = "ink_token";

// 应用挂载前缀，与 next.config 的 basePath 同源（NEXT_PUBLIC_BASE_PATH）。
//   ""        独立域名根路径部署（dualcon.xiaowuai.vip）
//   "/admin"  门户反代子路径部署（dual.xiaowuai.vip/admin，默认）
// 浏览器端以 "/" 开头的接口/资源路径需要手动补上该前缀，否则会打到站点根路径（门户）。
export const BASE_PATH = process.env.NEXT_PUBLIC_BASE_PATH ?? "/admin";

export function withBasePath(path: string) {
  return path.startsWith("/") && !path.startsWith("//") ? `${BASE_PATH}${path}` : path;
}

export function getToken() {
  return typeof window === "undefined" ? null : localStorage.getItem(TOKEN_KEY);
}

export function setToken(token: string) {
  localStorage.setItem(TOKEN_KEY, token);
}

export function clearToken() {
  localStorage.removeItem(TOKEN_KEY);
}

export function authHeaders(extra: Record<string, string> = {}) {
  const token = getToken();
  return token ? { ...extra, Authorization: `Bearer ${token}` } : extra;
}

export async function api<T = Record<string, unknown>>(path: string, init: RequestInit = {}): Promise<T> {
  const response = await fetch(withBasePath(path), {
    ...init,
    headers: authHeaders(init.headers as Record<string, string> | undefined),
  });
  const data = await response.json().catch(() => ({}));
  if (!response.ok) {
    const message = data.detail || data.error || data.message || `请求失败 (${response.status})`;
    throw new Error(typeof message === "string" ? message : "请求失败");
  }
  return data as T;
}
