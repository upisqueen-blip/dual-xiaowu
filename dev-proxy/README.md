# 本地域名复刻环境（dev-proxy）

用**域名**而不是**端口**区分本地服务，让本地拓扑与线上 `dual.xiaowuai.vip` / `dualcn.xiaowuai.vip` 一致。

## 为什么需要它

本地原来靠端口区分（8101 / 8094 / 8104），线上靠域名 + 路径区分。这两种模型有四件事**不等价**，而它们恰好是「本地好好的、线上就炸」的常见来源：

| 差异点 | 端口模型（旧） | 域名模型（线上 / 现在本地也这样） |
|---|---|---|
| Cookie 域 | 各端口**共享** cookie（cookie 不看端口） | 两个子域各存一份，登录态不互通 |
| 同源判定 | `http://localhost:8101` 与 `:8094` 天然同源判定简单 | 依赖 `X-Forwarded-Host/Proto` 头，反代漏传就 403 |
| 路径前缀 | 根路径 | 门户有 `/center`、`/backend`、`/admin` 前缀 |
| 协议 | http | https（`Secure` cookie、混合内容、wss） |

前两条决定了「登录态、跨域、跳转」这类问题能不能在本地复现；后两条决定「路径拼接、静态资源 404」能不能在本地复现。

## 三条命令跑起来

```bat
REM 1) 后端（8104）+ 两个前端（8094 / 8101）
start_all.bat

REM 2) 切到域名模式（写两个 app 的 .env.local）
switch-env.bat domain

REM 3) 起域名代理（保持窗口开着）
dev-proxy\start.bat
```

然后访问：

| 地址 | 对应线上 | 服务 |
|---|---|---|
| http://dual.localhost/ | https://dual.xiaowuai.vip/ | 创作者门户（8101） |
| http://dual.localhost/center | https://dual.xiaowuai.vip/center | 运营后台（8101） |
| http://dualcn.localhost/ | https://dualcn.xiaowuai.vip/ | 内容配置台（8094） |
| http://localhost/ | — | 路由表 + 上游健康状态 |

后端 **不出现在域名里**，只被 `/backend`、`/api` 反代——和线上完全一致。

## 文件说明

| 文件 | 作用 |
|---|---|
| `server.js` | 按 Host 分流的反代：HTTP + WebSocket(HMR)，透传 `X-Forwarded-*` |
| `routes.json` | 域名 → 端口映射，**改完保存即生效**，不用重启 |
| `start.bat` / `stop.bat` | 启停（监听 80 / 443） |
| `add-hosts.bat` | 仅当浏览器不认 `*.localhost` 时使用，需管理员权限 |
| `certs/` | 可选：放入 mkcert 证书即自动启用 HTTPS |

## 为什么不用改 hosts

Chrome / Edge / Firefox 会把任意 `*.localhost` 直接解析到 `127.0.0.1`，不需要管理员权限，也不影响你访问真正的线上域名。

如果想连 HTTPS 也一起测（`Secure` cookie、混合内容、wss），把 mkcert 生成的两张证书放进来即可：

```bat
mkcert -install
mkcert -cert-file certs/cert.pem -key-file certs/key.pem dual.localhost dualcn.localhost
```

重启 `start.bat`，就能用 `https://dual.localhost/`。要测「真实域名 + 真实证书」时才需要改 hosts 指向 `dual.xiaowuai.vip`——不建议日常这么做，会挡住你访问线上。

## 常见问题

**80 端口被占用**：`stop.bat` 先清一遍；仍被占用就用 `set DEV_PROXY_PORT=8080 && start.bat`，此时地址变成 `http://dual.localhost:8080/`（域名判定仍然生效，只是 origin 多了端口）。

**打开域名显示「上游没有响应」**：对应端口没起。代理不用重启，启动服务后刷新即可。

**页面能开但一直转圈 / 热更新失效**：`ALLOWED_DEV_ORIGINS` 没带域名。`switch-env.bat domain` 会写进 `.env.local`，之后**必须重启两个 Next dev**。

**改了 `switch-env` 没效果**：Next 的 `.env.local` 只在启动时读；`NEXT_PUBLIC_*` 还是构建期内联的，改完要重启 dev。

## 落地到线上前的自查清单

这六条在本地都可能"看起来正常"，但线上会出问题：

1. **`XIAOWU_PUBLIC_BASE_URL`**（`xiaowu_platform/settings.py:32`，默认 `http://127.0.0.1:8104`）
   用于 OAuth 回调 `redirect_uri`。线上必须设成对外地址，例如 `https://dual.xiaowuai.vip/backend`，否则第三方授权会回跳到 127.0.0.1。

2. **`/admin` 与 `basePath` 的双前缀**
   `pc-console` 的 rewrites 是 `/admin/:path*` → `${contentConsole}/admin/:path*`（转发后仍带 `/admin`），只有 `content-console` 以 `NEXT_PUBLIC_BASE_PATH=/admin` 构建时才匹配；而它作为独立域名 `dualcn` 部署时 `basePath=""`。
   **建议**：线上不要让门户反代 `/admin`，改为 302 到 `https://dualcn.xiaowuai.vip/`。一个入口只对应一种挂载位置，避免同一份产物被要求服务两种路径。

3. **反代必须透传 `X-Forwarded-Proto` / `X-Forwarded-Host`**
   后端 `OriginValidationMiddleware` 用它们判同源。nginx 漏配时，https 页面发出的请求会被判成 http 源，直接 403 `origin_not_allowed`。

4. **`INKSIGHT_ALLOWED_HOSTS`**
   后端 `TrustedHostMiddleware` 默认只放行 `localhost` / `127.0.0.1` / `insane.vip`，不含 `dual.xiaowuai.vip`。线上必须显式加上，否则整站 400 `Invalid host header`。

5. **`INKSIGHT_CORS_ORIGINS`**
   默认白名单是 3000 / 8090 / 8091 老端口。跨子域请求需要显式加入 `https://dual.xiaowuai.vip,https://dualcn.xiaowuai.vip`。

6. **会话 cookie 是 host-only**（`core/auth.py:129`，未设 `domain` 与 `secure`）
   两个子域各存一份会话。若要在 `dual` 登录后直接打开 `dualcn` 也算登录，需要把 cookie 域设为 `.xiaowuai.vip` 并加 `secure=True`（仅 https 生效）——注意这会让本地 http 调试失效，需按环境分支处理。
