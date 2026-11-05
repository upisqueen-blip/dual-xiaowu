#!/usr/bin/env node
'use strict';

// 本地域名复刻代理
// ---------------------------------------------------------------------------
// 目的：让本地访问方式和线上一致 —— 用「域名」而不是「端口」区分服务。
//
//   线上                                    本地
//   https://dual.xiaowuai.vip/             http://dual.localhost/
//   https://dual.xiaowuai.vip/center       http://dual.localhost/center
//   https://dual.xiaowuai.vip/backend/*    http://dual.localhost/backend/*
//   https://dualcn.xiaowuai.vip/           http://dualcn.localhost/
//
// 后端始终不出现在域名里，只被 Next 反代 —— 与线上完全同构。
// 代理会把 X-Forwarded-Host / -Proto / -For 透传给上游（线上 nginx 也这么做），
// 所以「靠反代头判断同源」这类只有线上才暴露的问题，本地也能提前撞到。
//
// 路由表在 dev-proxy/routes.json，改动即时生效，无需重启。
// 凭据：把 mkcert 生成的 cert.pem / key.pem 放进 dev-proxy/certs/ 即自动开 HTTPS。
// ---------------------------------------------------------------------------

const http = require('node:http');
const https = require('node:https');
const net = require('node:net');
const fs = require('node:fs');
const path = require('node:path');

const HTTP_PORT = Number(process.env.DEV_PROXY_PORT || 80);
const HTTPS_PORT = Number(process.env.DEV_PROXY_TLS_PORT || 443);
const UPSTREAM_HOST = '127.0.0.1';
const DIR = __dirname;
const ROUTES_FILE = process.env.DEV_PROXY_ROUTES || path.join(DIR, 'routes.json');
const CERT_DIR = path.join(DIR, 'certs');

const C = {
  reset: '\x1b[0m', dim: '\x1b[90m', red: '\x1b[31m',
  green: '\x1b[32m', yellow: '\x1b[33m', cyan: '\x1b[36m',
};

// 路由表热加载：每次请求只 stat 一次，改完 routes.json 立刻生效。
let routesCache = { mtimeMs: 0, routes: {} };

function loadRoutes() {
  try {
    const stat = fs.statSync(ROUTES_FILE);
    if (stat.mtimeMs === routesCache.mtimeMs) return routesCache.routes;

    const raw = JSON.parse(fs.readFileSync(ROUTES_FILE, 'utf8'));
    const routes = {};
    for (const [host, value] of Object.entries(raw)) {
      if (host.startsWith('_')) continue;
      if (!value || typeof value !== 'object' || !value.port) continue;
      routes[host.toLowerCase()] = { port: Number(value.port), label: value.label || '' };
    }
    routesCache = { mtimeMs: stat.mtimeMs, routes };
    const list = Object.entries(routes)
      .map(([host, route]) => `${host} -> 127.0.0.1:${route.port}`)
      .join(' , ');
    console.log(`${C.dim}[routes] ${Object.keys(routes).length} 条：${list}${C.reset}`);
    return routes;
  } catch (err) {
    console.error(`${C.red}[routes] 读取 routes.json 失败：${err.message}${C.reset}`);
    return routesCache.routes;
  }
}

function hostnameOf(req) {
  const header = req.headers.host || '';
  const host = header.split(':')[0].trim().toLowerCase();
  return host;
}

function resolveRoute(req) {
  const host = hostnameOf(req);
  if (!host) return null;
  const routes = loadRoutes();
  const route = routes[host];
  return route ? { ...route, host } : null;
}

function isStatusHost(host) {
  return host === 'localhost' || host === '127.0.0.1' || host === '::1' || host === '';
}

// ---------------------------------------------------------------------------
// 状态页：不知道域名怎么走的时候，打开 http://localhost/ 看这张表
// ---------------------------------------------------------------------------
function probe(port) {
  return new Promise((resolve) => {
    const socket = net.connect({ port, host: UPSTREAM_HOST });
    const done = (alive) => { socket.destroy(); resolve(alive); };
    socket.setTimeout(600, () => done(false));
    socket.on('connect', () => done(true));
    socket.on('error', () => done(false));
  });
}

async function renderStatusPage() {
  const routes = loadRoutes();
  const rows = [];
  for (const [host, route] of Object.entries(routes)) {
    const alive = await probe(route.port);
    rows.push(`
      <tr>
        <td><a href="http://${host}/">${host}</a></td>
        <td class="mono">127.0.0.1:${route.port}</td>
        <td><span class="${alive ? 'up' : 'down'}">${alive ? '运行中' : '未启动'}</span></td>
        <td class="dim">${route.label || ''}</td>
      </tr>`);
  }
  return `<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>本地域名代理 · 路由表</title>
<style>
  body{margin:0;padding:40px;background:#f7f7f5;color:#1c1c1a;
       font:14px/1.6 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif}
  h1{font-size:18px;font-weight:500;margin:0 0 4px}
  p.sub{color:#6b6b66;margin:0 0 24px;font-size:13px}
  table{border-collapse:collapse;width:100%;max-width:900px;background:#fff;
        border:1px solid #e3e3de;border-radius:10px;overflow:hidden}
  th,td{text-align:left;padding:10px 14px;border-bottom:1px solid #efeee9;font-size:13px}
  th{background:#faf9f6;font-weight:500;color:#6b6b66}
  tr:last-child td{border-bottom:0}
  a{color:#185fa5;text-decoration:none}
  a:hover{text-decoration:underline}
  .mono{font-family:ui-monospace,Consolas,monospace}
  .up{color:#0f6e56}.down{color:#a32d2d}
  .dim{color:#8a8a84}
  .tip{margin-top:20px;padding:12px 14px;background:#fff;border:1px solid #e3e3de;
       border-radius:10px;color:#6b6b66;font-size:12.5px;max-width:900px}
  code{font-family:ui-monospace,Consolas,monospace;background:#f1f0ec;padding:1px 5px;border-radius:4px}
</style></head><body>
<h1>本地域名代理</h1>
<p class="sub">用域名区分服务，与线上 dual / dualcn 同构。routes.json 改完即时生效。</p>
<table>
  <thead><tr><th>本地域名</th><th>上游</th><th>状态</th><th>用途</th></tr></thead>
  <tbody>${rows.join('')}</tbody>
</table>
<p class="tip">这些域名由浏览器内置解析到 127.0.0.1（Chrome / Edge / Firefox 均支持 <code>*.localhost</code>）。
若打不开，用管理员权限运行 <code>dev-proxy\add-hosts.bat</code> 追加 hosts 记录。
后端不出现在域名里，它只被 Next 的 <code>/backend</code>、<code>/api</code> 反代，和线上一致。</p>
</body></html>`;
}

function sendStatusPage(res) {
  renderStatusPage().then((html) => {
    res.writeHead(200, { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store' });
    res.end(html);
  }).catch((err) => {
    res.writeHead(500, { 'content-type': 'text/plain; charset=utf-8' });
    res.end('状态页渲染失败：' + err.message);
  });
}

function sendUpstreamDown(res, route) {
  const html = `<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<title>上游未启动</title><style>
body{margin:0;height:100vh;display:flex;align-items:center;justify-content:center;
     background:#f7f7f5;color:#1c1c1a;font:14px/1.7 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif}
.card{max-width:560px;padding:28px 30px;background:#fff;border:1px solid #e3e3de;border-radius:12px}
h1{font-size:16px;font-weight:500;margin:0 0 12px}
code{font-family:ui-monospace,Consolas,monospace;background:#f1f0ec;padding:1px 6px;border-radius:4px}
p{margin:8px 0;color:#4a4a45}p.dim{color:#8a8a84;font-size:12.5px}
</style></head><body><div class="card">
<h1>${route.host} 的上游没有响应</h1>
<p>代理要把请求转到 <code>127.0.0.1:${route.port}</code>，但那个端口没有在监听。</p>
<p>${route.label || ''}</p>
<p class="dim">先启动对应服务（<code>start_all.bat</code>），再刷新本页。代理本身不用重启。</p>
</div></body></html>`;
  res.writeHead(502, { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store' });
  res.end(html);
}

function sendUnknownHost(res, host) {
  const html = `<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<title>未配置的域名</title><style>
body{margin:0;height:100vh;display:flex;align-items:center;justify-content:center;
     background:#f7f7f5;color:#1c1c1a;font:14px/1.7 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif}
.card{max-width:560px;padding:28px 30px;background:#fff;border:1px solid #e3e3de;border-radius:12px}
h1{font-size:16px;font-weight:500;margin:0 0 12px}
code{font-family:ui-monospace,Consolas,monospace;background:#f1f0ec;padding:1px 6px;border-radius:4px}
a{color:#185fa5}p{margin:8px 0;color:#4a4a45}
</style></head><body><div class="card">
<h1>没有为 <code>${host}</code> 配置上游</h1>
<p>打开 <a href="http://localhost/">http://localhost/</a> 查看当前路由表，
或编辑 <code>dev-proxy/routes.json</code> 添加一条（保存即生效）。</p>
</div></body></html>`;
  res.writeHead(404, { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store' });
  res.end(html);
}

// ---------------------------------------------------------------------------
// HTTP 反代
// ---------------------------------------------------------------------------
function forwardHeaders(req) {
  const headers = { ...req.headers };
  // Host 原样透传：Next 的 allowedDevOrigins 校验、后端的同源判断都看它。
  headers['x-forwarded-host'] = req.headers.host || '';
  headers['x-forwarded-proto'] = req.socket.encrypted ? 'https' : 'http';
  const prior = req.headers['x-forwarded-for'];
  const ip = req.socket.remoteAddress || '';
  headers['x-forwarded-for'] = prior ? `${prior}, ${ip}` : ip;
  const port = req.socket.localPort;
  if (port) headers['x-forwarded-port'] = String(port);
  return headers;
}

function logLine(host, method, url, status, ms) {
  const color = status >= 500 ? C.red : status >= 400 ? C.yellow : C.green;
  console.log(
    `${C.cyan}${host}${C.reset} ${method} ${url} ` +
    `${color}${status}${C.reset} ${C.dim}${ms}ms${C.reset}`,
  );
}

function handleRequest(req, res) {
  const host = hostnameOf(req);

  if (isStatusHost(host) && (req.url === '/' || req.url === '/__routes')) {
    return sendStatusPage(res);
  }

  const route = resolveRoute(req);
  if (!route) return sendUnknownHost(res, host);

  const started = Date.now();
  const upstream = http.request(
    {
      host: UPSTREAM_HOST,
      port: route.port,
      method: req.method,
      path: req.url,
      headers: forwardHeaders(req),
    },
    (upstreamRes) => {
      res.writeHead(upstreamRes.statusCode || 502, upstreamRes.headers);
      upstreamRes.pipe(res);
      upstreamRes.on('end', () => logLine(route.host, req.method, req.url,
        upstreamRes.statusCode || 502, Date.now() - started));
    },
  );

  upstream.on('error', (err) => {
    logLine(route.host, req.method, req.url, 502, Date.now() - started);
    if (!res.headersSent) sendUpstreamDown(res, route);
    else res.destroy();
    if (err.code !== 'ECONNREFUSED') {
      console.error(`${C.red}[proxy] ${route.host} -> ${err.code || err.message}${C.reset}`);
    }
  });

  req.pipe(upstream);
}

// ---------------------------------------------------------------------------
// WebSocket / HMR 升级
// ---------------------------------------------------------------------------
function handleUpgrade(req, socket, head) {
  const route = resolveRoute(req);
  if (!route) {
    socket.end('HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n');
    return;
  }

  const upstream = net.connect({ host: UPSTREAM_HOST, port: route.port }, () => {
    const lines = [`${req.method} ${req.url} HTTP/1.1`];
    for (let i = 0; i < req.rawHeaders.length; i += 2) {
      lines.push(`${req.rawHeaders[i]}: ${req.rawHeaders[i + 1]}`);
    }
    upstream.write(`${lines.join('\r\n')}\r\n\r\n`);
    if (head && head.length) upstream.write(head);
    upstream.pipe(socket);
    socket.pipe(upstream);
    console.log(`${C.cyan}${route.host}${C.reset} WS ${req.url} ${C.dim}upgrade -> ${route.port}${C.reset}`);
  });

  upstream.on('error', () => socket.destroy());
  socket.on('error', () => upstream.destroy());
}

// ---------------------------------------------------------------------------
// 启动
// ---------------------------------------------------------------------------
function startServer(server, port, scheme) {
  server.on('error', (err) => {
    if (err.code === 'EADDRINUSE') {
      console.error(`${C.red}[fatal] ${scheme} 端口 ${port} 已被占用。${C.reset}`);
      console.error(`${C.dim}        可用 DEV_PROXY_PORT=8080 换端口启动（此时访问需带 :8080，`);
      console.error(`        域名仍生效，只是 origin 变成 http://dual.localhost:8080）。${C.reset}`);
      console.error(`${C.dim}        查看占用者：netstat -ano | findstr :${port}${C.reset}`);
    } else {
      console.error(`${C.red}[fatal] ${scheme} 端口 ${port} 监听失败：${err.message}${C.reset}`);
    }
    process.exit(1);
  });

  server.listen(port, () => {
    console.log(`${C.green}[ready]${C.reset} ${scheme} 监听 :${port}`);
  });
}

loadRoutes();
const httpServer = http.createServer(handleRequest);
httpServer.on('upgrade', handleUpgrade);
startServer(httpServer, HTTP_PORT, 'http');

const certFile = path.join(CERT_DIR, 'cert.pem');
const keyFile = path.join(CERT_DIR, 'key.pem');
if (fs.existsSync(certFile) && fs.existsSync(keyFile)) {
  const httpsServer = https.createServer(
    { cert: fs.readFileSync(certFile), key: fs.readFileSync(keyFile) },
    handleRequest,
  );
  httpsServer.on('upgrade', handleUpgrade);
  startServer(httpsServer, HTTPS_PORT, 'https');
} else {
  console.log(`${C.dim}[https] 未找到 dev-proxy/certs/{cert,key}.pem，跳过 443（需要时见 README）${C.reset}`);
}

console.log(`${C.dim}        路由表：http://localhost/   （或 /__routes）${C.reset}`);
