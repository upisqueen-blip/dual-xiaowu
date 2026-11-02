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

hdr "4. 端口监听（只列 node 相关，便于对照实际端口）"
if command -v ss >/dev/null 2>&1; then
  ss -lntp 2>/dev/null | grep -E ':(8094|8101|8104|3000|3001|8080|8081)\b|node' || echo "(无匹配)"
elif command -v netstat >/dev/null 2>&1; then
  netstat -lntp 2>/dev/null | grep -E ':(8094|8101|8104|3000|3001|8080|8081)\b|node' || echo "(无匹配)"
else
  echo "(ss/netstat 都没有)"
fi

# 端口优先从各自 package.json 的 start/dev 脚本里读，读不到再回落到默认值
port_of() {
  local app="$1" fallback="$2" pj="$ROOT/apps/$1/package.json"
  local p=""
  if [ -f "$pj" ] && command -v node >/dev/null 2>&1; then
    p="$(node -e "try{const s=require('$pj').scripts||{};const t=(s.start||'')+' '+(s.dev||'');const m=/-p\s*(\d+)/.exec(t);process.stdout.write(m?m[1]:'')}catch(e){}" 2>/dev/null)"
  fi
  [ -n "$p" ] && echo "$p" || echo "$fallback"
}
CC_PORT="$(port_of content-console-cn 8094)"
PC_PORT="$(port_of pc-console 8101)"
echo "  识别到的端口: content-console-cn=$CC_PORT  pc-console=$PC_PORT"

hdr "5. 逐级探测"
probe() {
  local url="$1" code
  code="$(curl -s -o /dev/null -w '%{http_code}' -m 6 "$url" 2>/dev/null || echo ERR)"
  printf '  %-44s -> %s\n' "$url" "$code"
}
probe "http://127.0.0.1:$CC_PORT/admin"
probe "http://127.0.0.1:$CC_PORT/admin/pair"
probe "http://127.0.0.1:$CC_PORT/"
probe "http://127.0.0.1:$PC_PORT/admin"
probe "http://127.0.0.1:$PC_PORT/admin/pair"
probe "http://127.0.0.1:$PC_PORT/"
probe "http://127.0.0.1:8104/health"

if [ $# -ge 1 ]; then
  hdr "6. 线上站点"
  BASE="${1%/}"
  probe "$BASE/admin"
  probe "$BASE/admin/pair"
  probe "$BASE/center"
  probe "$BASE/"
  echo "  --- /admin 响应头（看是谁返回的 404）---"
  curl -s -o /dev/null -D - -m 10 "$BASE/admin" 2>/dev/null | head -12
fi

hdr "判读"
cat <<EOF
  先看第 6 节的 4 个状态码，按下面顺序对号：

  1) 域名/admin=404  但  域名/admin/pair=200
     → 反代本身是通的，只是「裸 /admin」这条没匹配上 rewrite
       （/admin/:path* 在部分 Next 版本不匹配零段）。
       新版 next.config.mjs 已显式补了 source: "/admin" 这一条 → 重新 build pc-console 即可。

  2) 域名/admin 与 域名/admin/pair 都 404，但 127.0.0.1:$PC_PORT/admin=200
     → 请求被外层拦掉了（nginx / 负载均衡 / CDN）。
       查 location /admin 是否指向了不存在的 upstream。

  3) 127.0.0.1:$PC_PORT/admin 也是 404
     → pc-console 的 rewrite 没注册。第 1 节里 XIAOWU_CONTENT_CONSOLE_URL 缺失，
       或填成 http://host:$CC_PORT/admin（多带 /admin，新版会自动剥，但要重新 build）。
       另看 pc-console 启动日志有没有「[pc-console] .env 缺少以下配置」。

  4) 127.0.0.1:$CC_PORT/admin=404 而 127.0.0.1:$CC_PORT/=200
     → 上游产物没带 basePath，重新 build 上游。

  5) 127.0.0.1:$CC_PORT 完全连不上（000/ERR）
     → 内容台没起，或端口与 .env 里填的不一致（第 4 节对照实际监听端口）。

  附：/admin/pair 是上游的另一个真实路由，专门用来看「反代通不通」与
      「上游 basePath 有没有生效」，别跳过这一条。
EOF
