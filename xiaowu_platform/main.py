import json
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Annotated

from fastapi import Depends, FastAPI, Header, HTTPException, Response, status
from fastapi.responses import FileResponse, HTMLResponse, RedirectResponse
from pydantic import BaseModel

from . import data_api, isv_spec, sandbox
from .models import (AdminLogin, ContentConfigIn, ContentPush, DeveloperAppCreate, DeveloperLogin,
                     DataSourceIn, DeveloperRegister, DeviceCapabilities, DeviceEvent, DeviceRegister,
                     GithubImportIn, Heartbeat, MobileContentPush, ModelConfigUpsert, PairingConfirmIn,
                     PairingStartIn, PasswordLogin, ReviewDecision, ReviewSubmit, SandboxAppCreate, TaskAck,
                     TemplateUpsert, WebhookCreate)
from .connector_api import router as connector_router
from .connectors import ConnectorRegistry, CredentialVault
from .plugin_runtime import PluginRegistry
from .settings import settings
from .store import Store
from .console_api import router as console_router
from .render_api import router as render_router

store = Store(settings.database_path)
# 开发者沙箱产物目录：与数据库同级的 data/sandbox/<app_id>/repo
sandbox_root = settings.database_path.parent / "sandbox"
plugin_registry = PluginRegistry(Path(__file__).resolve().parents[1] / "plugins")
connector_registry = ConnectorRegistry(Path(__file__).resolve().parents[1] / "connectors")
credential_vault = CredentialVault(settings.connector_master_key)


@asynccontextmanager
async def lifespan(_: FastAPI):
    store.init()
    store.ensure_admin(settings.admin_username, settings.admin_password)
    yield


app = FastAPI(title="XiaoWu S3 Open Platform", version="0.1.0", lifespan=lifespan)

app.include_router(console_router)
app.include_router(render_router)
app.include_router(connector_router)


def current_app(authorization: Annotated[str | None, Header()] = None,
                x_app_key: Annotated[str | None, Header()] = None) -> dict:
    token = x_app_key or ""
    if authorization and authorization.lower().startswith("bearer "):
        token = authorization[7:].strip()
    app_info = store.authenticate_app(token)
    if not app_info:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "invalid app key")
    return app_info


def require_scope(app_info: dict, scope: str) -> None:
    if scope not in app_info["scopes"]:
        raise HTTPException(status.HTTP_403_FORBIDDEN, f"missing scope: {scope}")


def require_device(device_id: str, x_device_token: Annotated[str | None, Header()] = None) -> None:
    if not x_device_token or not store.authenticate(device_id, x_device_token):
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "invalid device token")


def current_user(x_user_token: Annotated[str | None, Header()] = None,
                 authorization: Annotated[str | None, Header()] = None) -> str:
    """用户会话：兼容 X-User-Token 与 Authorization: Bearer（前者给固件/小程序，后者给内容后台）。"""
    token = (x_user_token or "").strip()
    if not token and authorization and authorization.lower().startswith("bearer "):
        token = authorization[7:].strip()
    user_id = store.session_user(token)
    if not user_id:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "invalid user token")
    return user_id


def current_developer(authorization: Annotated[str | None, Header()] = None) -> dict:
    token = authorization[7:].strip() if authorization and authorization.lower().startswith("bearer ") else ""
    developer = store.developer_by_session(token)
    if not developer:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "invalid developer session")
    return developer


def require_admin(x_admin_key: Annotated[str | None, Header()] = None,
                  authorization: Annotated[str | None, Header()] = None) -> str:
    if x_admin_key == settings.admin_key:
        return "admin-key"
    token = authorization[7:].strip() if authorization and authorization.lower().startswith("bearer ") else ""
    admin = store.admin_by_session(token)
    if not admin:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "invalid admin credentials")
    return admin["admin_id"]


def require_device_header(device_id: str,
                          x_device_token: Annotated[str | None, Header()] = None) -> str:
    """固件侧鉴权：X-Device-Token（由 POST /api/device/{mac}/token 领取）。

    与 render / dual-config 同一套令牌体系，区别于注册时下发的注册令牌。
    """
    mac = (device_id or "").strip().upper()
    if not (x_device_token and store.verify_device_token(mac, x_device_token)):
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "invalid device token")
    return mac


@app.get("/", response_class=HTMLResponse)
def index() -> str:
    return """<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><title>森穹小悟 S3 平台后端</title>
<style>body{font-family:system-ui,-apple-system,"Segoe UI",sans-serif;max-width:720px;margin:48px auto;padding:0 20px;color:#1c2b24;background:#fbfcf8}
h1{font-size:22px}code{background:#eef1e9;padding:2px 6px;border-radius:4px;font-size:13px}
li{margin:6px 0}.tip{background:#eef1e9;border-radius:8px;padding:12px 16px}</style></head><body>
<h1>森穹小悟 S3 平台 · API 后端</h1>
<p>这是纯 API 服务，没有管理界面。请在浏览器打开 <b>PC 后台前端</b>（Next.js）：</p>
<div class="tip">cd apps/pc-console &amp;&amp; npm run dev &rarr; <code>http://127.0.0.1:8101</code><br>
内容配置页面：<code>http://127.0.0.1:8101/content-config</code></div>
<h3>常用接口</h3>
<ul>
<li>健康检查：<code>GET /health</code></li>
<li>接口文档（Swagger）：<code><a href="/docs">/docs</a></code></li>
<li>小程序内容配置：<code>GET/POST /api/v1/mobile/devices/{id}/content-config</code></li>
<li>PC 后台内容配置：<code>GET/POST /admin/v1/devices/{id}/content-config</code>（Header: <code>X-Admin-Key</code>）</li>
</ul>
</body></html>"""


@app.get("/health")
def health() -> dict[str, str]:
    return {"status": "ok", "platform": "xiaowu-s3"}


@app.post("/api/v1/devices/register", status_code=201)
def register(body: DeviceRegister, x_admin_key: Annotated[str | None, Header()] = None) -> dict[str, str]:
    if not settings.dev_mode and x_admin_key != settings.admin_key:
        raise HTTPException(status.HTTP_403_FORBIDDEN, "factory enrollment required")
    try:
        token = store.register(body.device_id, body.model, body.firmware_version, DeviceCapabilities().model_dump())
    except Exception as exc:
        if "UNIQUE" in str(exc):
            raise HTTPException(status.HTTP_409_CONFLICT, "device already registered") from exc
        raise
    if settings.dev_mode:
        store.bind_demo_device("demo-user", body.device_id)
    return {"device_id": body.device_id, "device_token": token}


@app.post("/api/v1/devices/{device_id}/heartbeat", status_code=204)
def heartbeat(device_id: str, body: Heartbeat, _: None = Depends(require_device)) -> Response:
    store.heartbeat(device_id, body.model_dump())
    return Response(status_code=204)


@app.get("/api/v1/devices/{device_id}/capabilities")
def capabilities(device_id: str, _: None = Depends(require_device)) -> dict:
    result = store.capabilities(device_id)
    if not result:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "device not found")
    return result


@app.get("/api/v1/devices/{device_id}/tasks/next")
def next_task(device_id: str, response: Response, _: None = Depends(require_device)) -> dict | None:
    task = store.next_task(device_id)
    if task is None:
        response.status_code = status.HTTP_204_NO_CONTENT
    return task


@app.post("/api/v1/devices/{device_id}/tasks/{task_id}/ack", status_code=204)
def ack(device_id: str, task_id: str, body: TaskAck, _: None = Depends(require_device)) -> Response:
    if not store.ack(device_id, task_id, body.status, body.detail):
        raise HTTPException(status.HTTP_404_NOT_FOUND, "task not found")
    return Response(status_code=204)


@app.post("/api/v1/devices/{device_id}/events", status_code=202)
def device_event(device_id: str, body: DeviceEvent, _: None = Depends(require_device)) -> dict[str, str]:
    event_id = store.record_device_event(device_id, body.event_type, body.data, body.occurred_at)
    return {"event_id": event_id, "status": "accepted"}


# ── 确权层（pairing）：ISV 申请一次性绑定码，用户在自己的后台显性点击「允许」──────────
# 设计要点：ISV 全程不接触用户账号与大模型凭证；平台只落一条 app↔device↔user 授权记录。
SCOPE_TEXT = {
    "device.content.write": "向你的设备推送内容",
    "device.read": "读取设备型号与状态",
}


# 对外 API 规范（公开、免鉴权）：开发者可在后台在线查看，也可直接下载喂给代码生成器/LLM。
@app.get("/openapi/isv.json", include_in_schema=False)
def isv_openapi_json(download: bool = False) -> Response:
    payload = json.dumps(isv_spec.build_spec(), ensure_ascii=False, indent=2)
    headers = ({"Content-Disposition": 'attachment; filename="xiaowu-isv-openapi.json"'}
               if download else {"Cache-Control": "no-cache"})
    return Response(content=payload, media_type="application/json; charset=utf-8", headers=headers)


@app.get("/openapi/isv.yaml", include_in_schema=False)
def isv_openapi_yaml(download: bool = False) -> Response:
    try:
        import yaml
    except ImportError:
        raise HTTPException(status.HTTP_503_SERVICE_UNAVAILABLE, "yaml renderer unavailable")

    class Dumper(yaml.SafeDumper):
        pass

    def represent_str(dumper, data):  # 多行文本用 | 块，避免 \n 转义成一行看不懂
        style = "|" if "\n" in data else None
        return dumper.represent_scalar("tag:yaml.org,2002:str", data, style=style)

    Dumper.add_representer(str, represent_str)
    payload = yaml.dump(isv_spec.build_spec(), Dumper=Dumper, allow_unicode=True,
                        sort_keys=False, width=120, default_flow_style=False)
    headers = ({"Content-Disposition": 'attachment; filename="xiaowu-isv-openapi.yaml"'}
               if download else {"Cache-Control": "no-cache"})
    return Response(content=payload, media_type="application/yaml; charset=utf-8", headers=headers)


@app.get("/openapi/isv.md", include_in_schema=False)
def isv_openapi_md(download: bool = False) -> Response:
    payload = isv_spec.to_markdown()
    headers = ({"Content-Disposition": 'attachment; filename="xiaowu-isv-api.md"'}
               if download else {"Cache-Control": "no-cache"})
    return Response(content=payload, media_type="text/markdown; charset=utf-8", headers=headers)


@app.post("/openapi/v1/pairing/start", status_code=201)
def pairing_start(body: PairingStartIn, app_info: dict = Depends(current_app)) -> dict:
    """ISV 申请一次性绑定码（默认 10 分钟有效）。

    display_code 由 ISV 展示给用户；用户在「小悟后台 → 授权管理」输入并显性确认后，
    ISV 再轮询 GET /openapi/v1/pairing/{code} 拿到 device_id 才能推送。
    """
    require_scope(app_info, "device.content.write")
    label = (body.label or app_info["name"])[:40]
    result = store.create_pairing_code(app_info["app_id"], "", label, body.scopes)
    return {
        "code": result["code"],
        "display_code": result["display_code"],
        "expires_in": result["expires_in"],
        "expires_at": result["expires_at"],
        "confirm_path": f"/pair?code={result['code']}",
        "confirm_hint": "请让用户登录小悟后台并打开『授权管理』，输入上方绑定码确认",
        "label": label,
        "scopes": body.scopes,
    }


@app.get("/openapi/v1/pairing/{code}")
def pairing_status(code: str, app_info: dict = Depends(current_app)) -> dict:
    """ISV 轮询绑定结果（pending/approved/denied/expired）。

    码必须属于当前应用，防止用别的应用的码窥探设备。
    """
    item = store.get_pairing_code(code)
    if not item or item["app_id"] != app_info["app_id"]:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "pairing code not found")
    approved = item["status"] == "approved"
    return {
        "code": item["code"],
        "status": item["status"],
        "device_id": item["device_id"] if approved else None,
        "grant_id": item["grant_id"] if approved else None,
        "expires_at": item["expires_at"],
    }


@app.post("/openapi/v1/devices/{device_id}/content", status_code=202)
def push_content(device_id: str, body: ContentPush, app_info: dict = Depends(current_app)) -> dict[str, str]:
    require_scope(app_info, "device.content.write")
    # 确权门禁：必须是用户显性授权过的设备，禁止凭 app key 向任意设备推送。
    grant = store.get_active_grant(app_info["app_id"], device_id)
    if not grant:
        raise HTTPException(status.HTTP_403_FORBIDDEN, {
            "error": "pairing_required",
            "message": "该设备尚未授权给此应用。请先调用 POST /openapi/v1/pairing/start，"
                       "让用户在『授权管理』输入绑定码确认后再推送。",
        })
    template = store.template(body.template)
    if not template:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "unknown or disabled template")
    missing = [field for field in template["schema_definition"].get("required", []) if body.data.get(field) in (None, "", [])]
    if missing:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, {"error": "template_validation_failed", "missing": missing})
    task_id = store.create_task(device_id, body.model_dump(exclude={"expires_in_seconds"}), body.expires_in_seconds,
                                source="openapi", created_by=app_info["app_id"])
    if task_id is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "device not found")
    store.touch_grant_push(grant["grant_id"])
    return {"task_id": task_id, "status": "queued"}


@app.get("/openapi/v1/devices/{device_id}")
def openapi_device(device_id: str, app_info: dict = Depends(current_app)) -> dict:
    require_scope(app_info, "device.read")
    capabilities = store.capabilities(device_id)
    if not capabilities:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "device not found")
    return {"device_id": device_id, "capabilities": capabilities}


@app.get("/openapi/v1/devices/{device_id}/events")
def openapi_events(device_id: str, app_info: dict = Depends(current_app)) -> list[dict]:
    require_scope(app_info, "device.event.read")
    return store.list_device_events(device_id)


@app.post("/openapi/v1/webhooks", status_code=201)
def create_webhook(body: WebhookCreate, app_info: dict = Depends(current_app)) -> dict[str, object]:
    require_scope(app_info, "webhook.manage")
    webhook_id, secret = store.create_webhook(app_info["app_id"], body.url, body.events)
    return {"id": webhook_id, "signing_secret": secret, "events": body.events}


@app.get("/openapi/v1/webhooks")
def list_webhooks(app_info: dict = Depends(current_app)) -> list[dict]:
    require_scope(app_info, "webhook.manage")
    return store.list_webhooks(app_info["app_id"])


# 开发者中心：账号、沙箱应用、模型配置与审核提交。
@app.post("/developer/v1/auth/register", status_code=201)
def developer_register(body: DeveloperRegister) -> dict[str, str]:
    if not body.accepted_terms or not body.accepted_content_policy:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, "必须同意开发者协议与内容安全规范")
    try:
        developer_id, token = store.register_developer(
            body.name, body.organization, body.email, body.password, body.agreement_version, body.role,
        )
    except ValueError as exc:
        raise HTTPException(status.HTTP_409_CONFLICT, str(exc)) from exc
    store.audit(developer_id, "developer.register", developer_id, {
        "agreement_version": body.agreement_version, "role": body.role,
    })
    return {"developer_id": developer_id, "token": token}


@app.post("/developer/v1/auth/login")
def developer_login(body: DeveloperLogin) -> dict[str, str]:
    result = store.login_developer(body.email, body.password)
    if not result:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "邮箱或密码错误")
    developer_id, token = result
    return {"developer_id": developer_id, "token": token}


@app.get("/developer/v1/me")
def developer_me(developer: dict = Depends(current_developer)) -> dict:
    return developer


@app.get("/developer/v1/hardware")
def developer_hardware() -> dict:
    return {
        "device": "小悟 S3 一体双屏", "refresh_strategy": "局部刷新优先，避免动画与高频刷新",
        "screens": [
            {"port": "FPC1", "id": "A", "type": "黑白电子墨水屏", "colors": ["black", "white"], "usage": "高频信息、文字与状态"},
            {"port": "FPC2", "id": "B", "type": "三色/四色全刷电子墨水屏", "colors": ["black", "white", "red", "yellow"], "usage": "低频海报、重点提示与氛围内容"},
        ],
        "rules": ["不依赖动画表达状态", "避免大面积连续反色", "提供纯黑白降级", "文字字号不小于 18px", "全刷内容建议间隔不少于 15 分钟"],
    }


def _attach_track(apps: list[dict]) -> list[dict]:
    """给应用列表补 track（static / data / none）与数据接口概要，供前端展示提交轨。"""
    for item in apps:
        ds = store.get_app_data_source(item["app_id"])
        if ds:
            item["track"] = "data"
            item["data_source"] = {
                "endpoint": ds.get("endpoint") or "",
                "verdict": ds.get("verdict") or "fail",
                "last_error": ds.get("last_error") or "",
                "checked_at": ds.get("checked_at") or "",
                "has_token": bool(ds.get("token")),
            }
        else:
            item["track"] = "static" if store.get_sandbox_build(item["app_id"]) else "none"
    return apps


@app.get("/developer/v1/apps")
def developer_apps(developer: dict = Depends(current_developer)) -> list[dict]:
    apps = store.list_sandbox_apps(developer["developer_id"])
    for item in apps:
        build = store.get_sandbox_build(item["app_id"])
        item["github"] = None if not build else {
            "repo_url": build["repo_url"], "commit_sha": build["commit_sha"],
            "entry": build["entry"], "verdict": build["verdict"],
            "imported_at": build["imported_at"], "preview_url": f"/sandbox/{item['app_id']}/",
            "report": build["report"],
        }
    return _attach_track(apps)


@app.post("/developer/v1/apps", status_code=201)
def developer_create_sandbox_app(body: SandboxAppCreate, developer: dict = Depends(current_developer)) -> dict:
    app_info, token = store.create_sandbox_app(
        developer["developer_id"], body.name, body.scene, body.description,
    )
    store.audit(developer["developer_id"], "sandbox_app.create", app_info["app_id"], {"scene": body.scene})
    return {**app_info, "sandbox_token": token}


@app.put("/developer/v1/apps/{app_id}/model")
def developer_model_save(app_id: str, body: ModelConfigUpsert,
                         developer: dict = Depends(current_developer)) -> dict:
    if not store.sandbox_app(app_id, developer["developer_id"]):
        raise HTTPException(status.HTTP_404_NOT_FOUND, "应用不存在")
    encrypted = credential_vault.encrypt({"api_key": body.api_key}) if body.api_key else ""
    store.save_model_config(app_id, body.provider, body.model, body.base_url, encrypted, body.system_prompt)
    store.audit(developer["developer_id"], "model_config.update", app_id, {"provider": body.provider, "model": body.model})
    return store.get_model_config(app_id) or {}


@app.get("/developer/v1/apps/{app_id}/model")
def developer_model_get(app_id: str, developer: dict = Depends(current_developer)) -> dict:
    if not store.sandbox_app(app_id, developer["developer_id"]):
        raise HTTPException(status.HTTP_404_NOT_FOUND, "应用不存在")
    return store.get_model_config(app_id) or {"api_key_configured": False}


@app.post("/developer/v1/apps/{app_id}/import-github", status_code=201)
def developer_import_github(app_id: str, body: GithubImportIn,
                            developer: dict = Depends(current_developer)) -> dict:
    """从 GitHub 公开仓库导入纯静态工程到沙箱，并按 4.2 寸墨水屏规则自动校验。"""
    app_info = store.sandbox_app(app_id, developer["developer_id"])
    if not app_info:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "应用不存在")
    if app_info["status"] == "in_review":
        raise HTTPException(status.HTTP_409_CONFLICT, "应用正在审核中，暂不可修改沙箱")
    try:
        build = sandbox.import_static_site(body.repo_url, sandbox_root / app_id, body.branch, body.entry)
    except sandbox.SandboxError as exc:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, str(exc)) from exc
    store.save_sandbox_build(app_id, build)
    store.audit(developer["developer_id"], "sandbox.github_import", app_id,
                {"repo": build["repo_url"], "commit": build["commit_sha"],
                 "verdict": build["report"]["verdict"]})
    return {
        "app_id": app_id, "repo_url": build["repo_url"], "repo_slug": build["repo_slug"],
        "branch": build["branch"], "commit_sha": build["commit_sha"], "entry": build["entry"],
        "file_count": build["file_count"], "size_bytes": build["size_bytes"],
        "report": build["report"], "preview_url": f"/sandbox/{app_id}/",
    }


@app.get("/developer/v1/apps/{app_id}/sandbox")
def developer_sandbox_build(app_id: str, developer: dict = Depends(current_developer)) -> dict:
    if not store.sandbox_app(app_id, developer["developer_id"]):
        raise HTTPException(status.HTTP_404_NOT_FOUND, "应用不存在")
    build = store.get_sandbox_build(app_id)
    if not build:
        return {"app_id": app_id, "imported": False, "preview_url": f"/sandbox/{app_id}/"}
    return {**build, "imported": True, "preview_url": f"/sandbox/{app_id}/"}


@app.get("/sandbox/{app_id}")
def sandbox_root_redirect(app_id: str):
    return RedirectResponse(url=f"/sandbox/{app_id}/", status_code=307)


@app.get("/sandbox/{app_id}/{file_path:path}")
def sandbox_static(app_id: str, file_path: str = ""):
    """托管开发者导入的静态工程：只读、限定在 data/sandbox/<app_id>/repo 内。"""
    build = store.get_sandbox_build(app_id)
    if not build:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "该应用尚未导入沙箱工程")
    root = sandbox_root / app_id / "repo"
    target = sandbox.resolve_asset(root, file_path or build.get("entry") or "index.html")
    if not target:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "资源不存在")
    return FileResponse(target, headers={"Cache-Control": "no-cache"})


@app.post("/developer/v1/apps/{app_id}/data-source")
def developer_save_data_source(app_id: str, body: DataSourceIn,
                               developer: dict = Depends(current_developer)) -> dict:
    """保存并校验「数据接口」（轨2）：实时抓取 ISV 端点，失败时用开发者提供的样本兜底。"""
    app_info = store.sandbox_app(app_id, developer["developer_id"])
    if not app_info:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "应用不存在")
    if app_info["status"] == "in_review":
        raise HTTPException(status.HTTP_409_CONFLICT, "应用正在审核中，暂不可修改数据接口")
    endpoint = body.endpoint.strip()
    token = body.token.strip()
    payload = None
    last_error = ""
    try:
        payload = data_api.fetch_card(endpoint, token, "A")
    except data_api.DataApiError as exc:
        last_error = str(exc)
        payload = body.sample
    if payload is None:
        base = data_api.validate_card({})
        checks = [{"id": "endpoint", "label": "端点可访问", "status": "fail",
                   "detail": last_error or "未取到数据：请检查端点地址与鉴权"}] + base["checks"]
        normalized: dict = {}
    else:
        base = data_api.validate_card(payload)
        checks = base["checks"]
        normalized = base["normalized"]
        if last_error:
            checks = [{"id": "endpoint", "label": "端点可访问", "status": "warn",
                       "detail": f"实时取数失败（{last_error}），已用样本校验；上线前请确保端点可访问"}] + checks
    failed = [c for c in checks if c["status"] == "fail"]
    warned = [c for c in checks if c["status"] == "warn"]
    verdict = "fail" if failed else ("warn" if warned else "pass")
    summary = {"pass": sum(1 for c in checks if c["status"] == "pass"),
               "warn": len(warned), "fail": len(failed)}
    sample_to_save = normalized if (verdict != "fail" and normalized) else None
    store.save_app_data_source(app_id, endpoint, token, verdict, checks, sample_to_save, last_error)
    store.audit(developer["developer_id"], "sandbox_app.data_source", app_id, {"verdict": verdict})
    return {"app_id": app_id, "verdict": verdict, "checks": checks, "summary": summary,
            "sample": normalized}


@app.get("/developer/v1/apps/{app_id}/data-source")
def developer_get_data_source(app_id: str, developer: dict = Depends(current_developer)) -> dict:
    if not store.sandbox_app(app_id, developer["developer_id"]):
        raise HTTPException(status.HTTP_404_NOT_FOUND, "应用不存在")
    ds = store.get_app_data_source(app_id)
    if not ds:
        return {"app_id": app_id, "configured": False}
    return {"app_id": app_id, "configured": True, "endpoint": ds.get("endpoint") or "",
            "has_token": bool(ds.get("token")), "verdict": ds.get("verdict") or "fail",
            "checks": ds.get("checks") or [], "sample": ds.get("sample") or {},
            "checked_at": ds.get("checked_at") or "", "last_error": ds.get("last_error") or ""}


# ── 接入凭证与确权联调（开发者自助，session 鉴权，免把 App Key 填进浏览器）──────────────
@app.post("/developer/v1/apps/{app_id}/credential", status_code=201)
def developer_issue_credential(app_id: str, developer: dict = Depends(current_developer)) -> dict:
    """为应用签发（或轮换）OpenAPI 凭证，app_id 与内容应用保持一致，确权/推送共用。"""
    app_info = store.sandbox_app(app_id, developer["developer_id"])
    if not app_info:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "应用不存在")
    result = store.issue_app_credential(app_id, app_info["name"])
    store.audit(developer["developer_id"], "app.credential.issue", app_id, {"scopes": result["scopes"]})
    return result


@app.get("/developer/v1/apps/{app_id}/credential")
def developer_get_credential(app_id: str, developer: dict = Depends(current_developer)) -> dict:
    if not store.sandbox_app(app_id, developer["developer_id"]):
        raise HTTPException(status.HTTP_404_NOT_FOUND, "应用不存在")
    app = store.app_public(app_id)
    return {
        "issued": bool(app and app.get("status") == "active"),
        "app_id": app_id,
        "scopes": (app or {}).get("scopes", []),
        "created_at": (app or {}).get("created_at"),
    }


@app.post("/developer/v1/apps/{app_id}/pairing/start", status_code=201)
def developer_pairing_start(app_id: str, body: PairingStartIn,
                            developer: dict = Depends(current_developer)) -> dict:
    """开发者后台「绑定测试」：用会话直接签发绑定码，自测确权闭环，无需在浏览器里放 App Key。"""
    app_info = store.sandbox_app(app_id, developer["developer_id"])
    if not app_info:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "应用不存在")
    if not store.app_public(app_id):
        raise HTTPException(status.HTTP_409_CONFLICT, "请先签发接入凭证（App Token），否则应用无法在设备端生效")
    label = (body.label or app_info["name"])[:40]
    result = store.create_pairing_code(app_id, developer["developer_id"], label, body.scopes)
    return {**result, "app_name": app_info["name"], "label": label, "scopes": body.scopes,
            "confirm_path": f"/pair?code={result['code']}"}


@app.get("/developer/v1/apps/{app_id}/pairing/{code}")
def developer_pairing_status(app_id: str, code: str,
                             developer: dict = Depends(current_developer)) -> dict:
    if not store.sandbox_app(app_id, developer["developer_id"]):
        raise HTTPException(status.HTTP_404_NOT_FOUND, "应用不存在")
    item = store.get_pairing_code(code)
    if not item or item["app_id"] != app_id:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "绑定码不存在")
    approved = item["status"] == "approved"
    return {"code": item["code"], "status": item["status"],
            "device_id": item["device_id"] if approved else None,
            "grant_id": item["grant_id"] if approved else None,
            "expires_at": item["expires_at"]}


@app.post("/developer/v1/apps/{app_id}/submit", status_code=202)
def developer_submit(app_id: str, body: ReviewSubmit, developer: dict = Depends(current_developer)) -> dict:
    build = store.get_sandbox_build(app_id)
    ds = store.get_app_data_source(app_id)
    if ds:
        if (ds.get("verdict") or "fail") == "fail":
            blocking = [c["label"] for c in (ds.get("checks") or []) if c.get("status") == "fail"]
            raise HTTPException(status.HTTP_409_CONFLICT,
                                "数据接口未通过校验：" + "；".join(blocking or ["请重新校验端点"])
                                + "。请修正后重新校验再提交")
    else:
        if not build:
            raise HTTPException(status.HTTP_409_CONFLICT,
                                "提交前需先完成「双屏适配UI（静态网页）」或「数据接口」其中之一")
        report = build.get("report") or {}
        if report.get("verdict") == "fail":
            blocking = [c["label"] for c in report.get("checks", []) if c.get("status") == "fail"]
            raise HTTPException(status.HTTP_409_CONFLICT,
                                "沙箱工程未通过墨水屏适配规则：" + "；".join(blocking) + "。请修正后重新导入再提交")
    if not store.submit_sandbox_app(app_id, developer["developer_id"], body.version, body.notes):
        raise HTTPException(status.HTTP_409_CONFLICT, "仅沙箱或已驳回应用可提交审核")
    store.audit(developer["developer_id"], "sandbox_app.submit", app_id, {"version": body.version})
    return {"app_id": app_id, "status": "in_review"}


# 小程序端：只提供用户自助内容管理，不暴露运营能力。
@app.post("/api/v1/mobile/login")
def mobile_login(body: PasswordLogin) -> dict[str, str]:
    result = store.login(body.username, body.password)
    if not result:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "invalid username or password")
    user_id, token = result
    return {"user_id": user_id, "token": token}


@app.get("/api/v1/mobile/scenes")
def mobile_scenes(locale: str = "zh-CN", _: str = Depends(current_user)) -> list[dict]:
    templates = store.list_templates(enabled_only=True)
    names = EN_TEMPLATE_NAMES if locale.lower().startswith("en") else {}
    return [{"id": item["template_id"], "name": names.get(item["template_id"], item["name"]), "category": item["category"],
             "schema": item["schema_definition"]} for item in templates]


@app.get("/api/v1/mobile/devices")
def mobile_devices(user_id: str = Depends(current_user)) -> list[dict]:
    return store.user_devices(user_id)


# 用户侧确权：绑定码必须由已登录用户本人显性确认（点「允许 / 拒绝」），
# 平台据此建立 app↔device↔user 三元授权；用户随时可在同一入口撤销。
@app.get("/api/v1/mobile/pairing/{code}")
def mobile_pairing_preview(code: str, user_id: str = Depends(current_user)) -> dict:
    item = store.get_pairing_code(code)
    if not item:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "binding code not found")
    app = store.app_public(item["app_id"]) or {}
    return {
        "code": item["code"],
        "status": item["status"],
        "expires_at": item["expires_at"],
        "app": {"app_id": item["app_id"], "name": app.get("name") or item["app_id"]},
        "label": item["label"],
        "scopes": [{"key": s, "text": SCOPE_TEXT.get(s, s)} for s in item["scopes"]],
        "devices": store.user_devices(user_id),
    }


@app.post("/api/v1/mobile/pairing/{code}/confirm", status_code=201)
def mobile_pairing_confirm(code: str, body: PairingConfirmIn,
                           user_id: str = Depends(current_user)) -> dict:
    try:
        grant = store.approve_pairing_code(code, user_id, body.device_id)
    except LookupError:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "binding code not found")
    except TimeoutError:
        raise HTTPException(status.HTTP_410_GONE, "binding code expired, please ask the app for a new one")
    except ValueError:
        raise HTTPException(status.HTTP_409_CONFLICT, "binding code already used")
    except PermissionError as exc:
        detail = ("device is not bound to this user" if str(exc) == "device_not_owned"
                  else "application has no grantable scope")
        raise HTTPException(status.HTTP_403_FORBIDDEN, detail)
    store.audit(user_id, "pairing.approve", body.device_id,
                {"app_id": grant["app_id"], "grant_id": grant["grant_id"]})
    return grant


@app.post("/api/v1/mobile/pairing/{code}/deny", status_code=204)
def mobile_pairing_deny(code: str, user_id: str = Depends(current_user)) -> Response:
    if not store.deny_pairing_code(code, user_id):
        raise HTTPException(status.HTTP_404_NOT_FOUND, "no pending binding code")
    store.audit(user_id, "pairing.deny", code, {})
    return Response(status_code=204)


@app.get("/api/v1/mobile/grants")
def mobile_grants(user_id: str = Depends(current_user)) -> list[dict]:
    rows = store.list_user_grants(user_id)
    for item in rows:
        item["scope_text"] = [SCOPE_TEXT.get(s, s) for s in item.get("scopes") or []]
    return rows


@app.delete("/api/v1/mobile/grants/{grant_id}", status_code=204)
def mobile_revoke_grant(grant_id: str, user_id: str = Depends(current_user)) -> Response:
    if not store.revoke_grant(grant_id, user_id):
        raise HTTPException(status.HTTP_404_NOT_FOUND, "grant not found")
    store.audit(user_id, "grant.revoke", grant_id, {})
    return Response(status_code=204)


@app.post("/api/v1/mobile/devices/{device_id}/content", status_code=202)
def mobile_push(device_id: str, body: MobileContentPush, user_id: str = Depends(current_user)) -> dict[str, str]:
    if not store.user_owns(user_id, device_id):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "device is not bound to this user")
    task_id = store.create_task(device_id, body.model_dump(exclude={"expires_in_seconds", "title"}),
                                body.expires_in_seconds, source="miniapp", created_by=user_id)
    assert task_id
    store.audit(user_id, "content.push", device_id, {"task_id": task_id, "template": body.template})
    return {"task_id": task_id, "status": "queued"}


@app.get("/api/v1/mobile/devices/{device_id}/history")
def mobile_history(device_id: str, user_id: str = Depends(current_user)) -> list[dict]:
    if not store.user_owns(user_id, device_id):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "device is not bound to this user")
    return store.task_history(device_id, 30)


# 设备留言：固件轮询待播留言 → 语音端播放 → 回执标记已听（Unread 徽标随之消失）
@app.get("/api/v1/devices/{device_id}/messages")
def device_messages(device_id: str, limit: int = 20, unread: bool = True,
                    mac: str = Depends(require_device_header)) -> dict:
    items = store.list_messages(mac, max(1, min(limit, 100)), unread_only=unread)
    return {
        "unread": store.count_unread_messages(mac),
        "items": [
            {
                "id": it["id"], "kind": it["kind"], "text": it.get("text") or "",
                "sender": it.get("sender_name") or "", "sender_type": it.get("sender_type") or "",
                "duration": it.get("duration") or 0,
                "created_at": it.get("created_at") or "",
                "played": bool(it.get("played_at")),
                # 语音端直接拉取该地址播放（需带同一枚 X-Device-Token）
                "audio_url": it.get("audio_url") or "",
            }
            for it in items
        ],
    }


@app.post("/api/v1/devices/{device_id}/messages/{message_id}/played", status_code=204)
def device_message_played(device_id: str, message_id: str,
                          mac: str = Depends(require_device_header)) -> Response:
    store.mark_message_played(mac, message_id)
    return Response(status_code=204)


# 双屏内容配置：小程序与 PC 后台读写同一份 dual_config，互不覆盖。
@app.get("/api/v1/mobile/devices/{device_id}/content-config")
def mobile_content_config(device_id: str, user_id: str = Depends(current_user)) -> dict:
    if not store.user_owns(user_id, device_id):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "device is not bound to this user")
    cfg = store.get_content_config(device_id)
    return cfg or {"config": None, "updated_at": None}


@app.post("/api/v1/mobile/devices/{device_id}/content-config", status_code=201)
def mobile_save_content_config(device_id: str, body: ContentConfigIn, user_id: str = Depends(current_user)) -> dict:
    if not store.user_owns(user_id, device_id):
        raise HTTPException(status.HTTP_403_FORBIDDEN, "device is not bound to this user")
    ts = store.save_content_config(device_id, body.payload)
    return {"status": "saved", "updated_at": ts}


# PC 运营后台：全局设备、内容模板、任务、插件与审计都仅在这里开放。
@app.post("/admin/v1/auth/login")
def admin_login(body: AdminLogin) -> dict:
    result = store.login_admin(body.username, body.password)
    if not result:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "管理员账号或密码错误")
    admin, token = result
    store.audit(admin["admin_id"], "admin.login", admin["admin_id"], {})
    return {**admin, "token": token}


@app.get("/admin/v1/auth/me")
def admin_me(actor: str = Depends(require_admin)) -> dict:
    return {"admin_id": actor}


@app.get("/admin/v1/overview")
def admin_overview(_: str = Depends(require_admin)) -> dict:
    return store.overview()


@app.get("/admin/v1/devices")
def admin_devices(_: str = Depends(require_admin)) -> list[dict]:
    return store.all_devices()


@app.get("/admin/v1/tasks")
def admin_tasks(_: str = Depends(require_admin)) -> list[dict]:
    return store.task_history(limit=200)


@app.get("/admin/v1/templates")
def admin_templates(_: str = Depends(require_admin)) -> list[dict]:
    return store.list_templates()


@app.put("/admin/v1/templates/{template_id}", status_code=204)
def admin_template_save(template_id: str, body: TemplateUpsert, actor: str = Depends(require_admin)) -> Response:
    if template_id != body.template_id:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "template id mismatch")
    store.upsert_template(body.model_dump(), actor)
    return Response(status_code=204)


@app.get("/admin/v1/audit-logs")
def admin_audits(_: str = Depends(require_admin)) -> list[dict]:
    return store.audits()


@app.get("/admin/v1/plugins")
def admin_plugins(_: str = Depends(require_admin)) -> list[dict]:
    return [manifest.public_dict() for manifest in plugin_registry.load()]


@app.post("/admin/v1/developer-apps", status_code=201)
def admin_create_app(body: DeveloperAppCreate, actor: str = Depends(require_admin)) -> dict[str, object]:
    app_id, token = store.create_developer_app(body.name, list(body.scopes))
    store.audit(actor, "developer_app.create", app_id, {"name": body.name, "scopes": body.scopes})
    return {"app_id": app_id, "token": token, "scopes": body.scopes}


@app.get("/admin/v1/developer-apps")
def admin_apps(_: str = Depends(require_admin)) -> list[dict]:
    return store.list_developer_apps()


@app.get("/admin/v1/creator-accounts")
def admin_creator_accounts(_: str = Depends(require_admin)) -> list[dict]:
    return store.list_developer_accounts()


@app.get("/admin/v1/developer-submissions")
def admin_developer_submissions(_: str = Depends(require_admin)) -> list[dict]:
    return _attach_track(store.list_sandbox_apps())


@app.post("/admin/v1/developer-submissions/{app_id}/decision")
def admin_developer_decision(app_id: str, body: ReviewDecision, actor: str = Depends(require_admin)) -> dict:
    if not store.review_sandbox_app(app_id, body.decision, body.note):
        raise HTTPException(status.HTTP_409_CONFLICT, "仅审核中的应用可处理")
    store.audit(actor, f"sandbox_app.{body.decision}", app_id, {"note": body.note})
    return {"app_id": app_id, "status": "published" if body.decision == "approve" else "rejected"}


@app.get("/admin/v1/published-apps")
def admin_published_apps(_: str = Depends(require_admin)) -> list[dict]:
    return store.list_published_apps()


class PublishedAppToggle(BaseModel):
    enabled: bool


@app.post("/admin/v1/published-apps/{app_id}/toggle")
def admin_toggle_published_app(app_id: str, body: PublishedAppToggle, _: str = Depends(require_admin)) -> dict:
    if not store.set_published_app_enabled(app_id, 1 if body.enabled else 0):
        raise HTTPException(status.HTTP_404_NOT_FOUND, "未找到已发布应用记录")
    return {"app_id": app_id, "enabled": bool(body.enabled)}


@app.get("/admin/v1/webhook-outbox")
def admin_webhook_outbox(_: str = Depends(require_admin)) -> list[dict]:
    return store.webhook_outbox()


# PC 后台：双屏内容配置读写（与小程序共用同一份数据）。
@app.get("/admin/v1/devices/{device_id}/content-config")
def admin_content_config(device_id: str, _: str = Depends(require_admin)) -> dict:
    cfg = store.get_content_config(device_id)
    return cfg or {"config": None, "updated_at": None}


@app.post("/admin/v1/devices/{device_id}/content-config", status_code=201)
def admin_save_content_config(device_id: str, body: ContentConfigIn, _: str = Depends(require_admin)) -> dict:
    if not store.device_exists(device_id):
        raise HTTPException(status.HTTP_404_NOT_FOUND, "device not found")
    ts = store.save_content_config(device_id, body.payload)
    return {"status": "saved", "updated_at": ts}


from fastapi.staticfiles import StaticFiles

_upload_dir = settings.database_path.parent / "uploads"
_upload_dir.mkdir(parents=True, exist_ok=True)
app.mount("/uploads", StaticFiles(directory=str(_upload_dir)), name="uploads")


EN_TEMPLATE_NAMES = {
    "notice-card": "General notice", "homework-list": "Homework list",
    "vocabulary-card": "Vocabulary card", "care-reminder": "Care reminder",
    "daily-schedule": "Daily schedule",
}
