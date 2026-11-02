#!/usr/bin/env bash
# 诊断「/admin 双屏内容配置 404」
#
# 用法（在仓库根目录或任意位置）：
#   bash scripts/diag-admin-404.sh                    # 只查本机三级链路
#   bash scripts/diag-admin-404.sh https://your.domain # 额外探测线上站点
#
# 判读区在最后，会直接告诉你 404 是哪一环断的。

set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

hdr() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }

hdr "0. 位置"
echo "repo: $ROOT"
command -v curl >/dev/null 2>&1 && echo "curl: ok" || echo "curl: 缺失（下面探测会失败）"

hdr "1. pc-console/.env —— 决定 /admin 反代到哪"
ENV_PC="$ROOT/apps/pc-console/.env"
if [ -f "$ENV_PC" ]; then
  grep -E '^[[:space:]]*(XIAOWU_API_BASE|XIAOWU_CONTENT_CONSOLE_URL)[[:space:]]*=' "$ENV_PC" \
    || echo "!! 两个变量都没配：/admin 与 /backend 都不会注册 rewrite，访问即 404"
else
  echo "!! 缺 $ENV_PC"
  echo "   pc-console 读不到 XIAOWU_CONTENT_CONSOLE_URL 就不会注册 /admin rewrite → 必然 404"
  echo "   修：cp apps/pc-console/.env.example apps/pc-console/.env 后填真实地址，再重启 pc-console"
fi

hdr "2. content-console-cn/.env —— 决定 /api 反代到哪"
ENV_CN="$ROOT/apps/content-console-cn/.env"
if [ -f "$ENV_CN" ]; then
  grep -E '^[[:space:]]*INKSIGHT_BACKEND_API_BASE[[:space:]]*=' "$ENV_CN" || echo "(未配：/api 反代不生效)"
else
  echo "!! 缺 $ENV_CN"
fi

hdr "3. content-console-cn 构建产物是否带 basePath=/admin"
MANIFEST="$ROOT/apps/content-console-cn/.next/routes-manifest.json"
if [ -f "$MANIFEST" ]; then
  if grep -q '"/admin' "$MANIFEST"; then
    echo "ok: routes-manifest 里出现 /admin 前缀"
  else
    echo "!! routes-manifest 里没有 /admin 前缀"
    echo "   说明这份 .next 是旧构建（改 next.config.ts 后没重新 build），上游收到 /admin 会 404"
    echo "   修：cd apps/content-console-cn && npm run build && 重启服务"
  fi
  echo "manifest 时间: $(date -r "$MANIFEST" '+%F %T' 2>/dev/null || echo unknown)"
else
  echo "!! 没有 .next/routes-manifest.json —— 还没执行 npm run build，或跑的是 next dev"
fi

hdr "4. 端口监听（8094 内容台 / 8101 门户 / 8104 后端）"
if command -v ss >/dev/null 2>&1; then
  ss -lntp 2>/dev/null | grep -E ':(8094|8101|8104)\b' || echo "(ss 无匹配)"
elif command -v netstat >/dev/null 2>&1; then
  netstat -lntp 2>/dev/null | grep -E ':(8094|8101|8104)\b' || echo "(netstat 无匹配)"
else
  echo "(ss/netstat 都没有)"
fi

hdr "5. 逐级探测"
probe() {
  local url="$1" code
  code="$(curl -s -o /dev/null -w '%{http_code}' -m 6 "$url" 2>/dev/null || echo ERR)"
  printf '  %-44s -> %s\n' "$url" "$code"
}
probe "http://127.0.0.1:8094/admin"
probe "http://127.0.0.1:8094/"
probe "http://127.0.0.1:8101/admin"
probe "http://127.0.0.1:8101/"
probe "http://127.0.0.1:8104/health"

if [ $# -ge 1 ]; then
  hdr "6. 线上站点"
  BASE="${1%/}"
  probe "$BASE/admin"
  probe "$BASE/"
  probe "$BASE/center"
  echo "  --- /admin 响应头（看是谁返回的 404）---"
  curl -s -o /dev/null -D - -m 10 "$BASE/admin" 2>/dev/null | head -12
fi

hdr "判读"
cat <<'EOF'
  A) 8094/admin=200 且 8101/admin=200
     → 两级服务都正常，404 来自更外层：nginx/网关的 location 规则或域名路由。
       检查 nginx 是否把 /admin 单独 match 到了不存在的 upstream。

  B) 8094/=200 但 8094/admin=404
     → 上游产物没带 basePath。改过 next.config.ts 之后必须重新 npm run build，
       只重启进程不会生效。见第 3 节。

  C) 8094/admin=200 但 8101/admin=404
     → pc-console 的 /admin rewrite 没注册或目标写错。见第 1 节，
       常见是把 XIAOWU_CONTENT_CONSOLE_URL 填成了 http://host:8094/admin（多带 /admin）。

  D) 8094 完全连不上（000/ERR）
     → 内容台没起，或端口不是 8094。确认 package.json 的 -p 与 .env 里填的端口一致。

  E) 线上 /admin 有响应但不是 200，响应头里 server 是 nginx
     → 请求根本没到 pc-console，直接查 nginx 配置。
EOF
