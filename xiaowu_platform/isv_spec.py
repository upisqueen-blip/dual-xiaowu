"""ISV OpenAPI 规范（对外接口的唯一真源）。

这里只描述第三方开发者真正会用到的那一层（/openapi/v1/*），不包含运营后台与设备固件接口。
同一份 dict 同时供给三种形态，避免多处手写导致不一致：
  - /openapi/isv.json  OpenAPI 3.0.3（给 Swagger UI / Postman / 代码生成器）
  - /openapi/isv.yaml  同上 YAML 版（给人读、给 LLM 贴）
  - /openapi/isv.md    人读版说明（可直接下载当接入备忘）

改接口时只改这个文件 + main.py 的路由，两处必须同步。
"""

from __future__ import annotations

from typing import Any

SPEC_VERSION = "1.0.0"
API_TITLE = "小悟双屏 · ISV 内容接入 API"
API_DESCRIPTION = """第三方应用把内容推到小悟墨水屏的接口集合。

接入顺序固定为四步：**签发凭证 → 确权绑定 → 交付数据 → 推送上线**。

关键约束：
1. 所有接口用 `X-App-Key` 鉴权，值就是开发者后台签发的 App Token（只显示一次）。
2. 推送前必须先完成确权：设备必须被**用户本人**显性授权给你的应用，否则返回 `403 pairing_required`。
   平台不接触用户的大模型账号与凭证，也无法代查任何第三方额度——取数在你那边完成，这里只接收结果。
3. 推送是**异步**的：`202` 只代表已入队，真实「已显示 / 失败」请查 `/events` 或配置 webhook。
4. 屏幕是墨水屏：400×300、无动画、字号 ≥18px、单屏信息 ≤6 条；同一内容不要高频重推（耗电、伤屏）。

错误统一形如 `{"error": "code", "message": "人话说明"}`，`error` 可用于程序判断，`message` 可用于展示。
"""

TAG_PAIRING = "① 确权绑定"
TAG_DEVICE = "② 设备信息"
TAG_CONTENT = "③ 内容推送"
TAG_EVENT = "④ 回执与事件"

_SCOPE_NOTE = {
    "device.content.write": "写入内容（推送卡片、申请绑定码）",
    "device.read": "读取设备能力与规格",
    "device.event.read": "读取设备事件回执",
}


def _err(desc: str, code: str, message: str) -> dict[str, Any]:
    return {
        "description": desc,
        "content": {"application/json": {
            "schema": {"$ref": "#/components/schemas/Error"},
            "example": {"error": code, "message": message},
        }},
    }


def _json(desc: str, schema: Any, example: Any | None = None) -> dict[str, Any]:
    body: dict[str, Any] = {"schema": schema}
    if example is not None:
        body["example"] = example
    return {"description": desc, "content": {"application/json": body}}


def build_spec() -> dict[str, Any]:
    return {
        "openapi": "3.0.3",
        "info": {
            "title": API_TITLE,
            "version": SPEC_VERSION,
            "description": API_DESCRIPTION,
            "contact": {"name": "小悟众创平台", "url": "/developer"},
            "license": {"name": "平台开发者服务协议 v2026-10-23"},
        },
        "servers": [{
            "url": "{baseUrl}",
            "description": "沙箱环境默认本地；生产地址由平台在接入工单中下发",
            "variables": {
                "baseUrl": {
                    "default": "http://127.0.0.1:8104",
                    "description": "后端根地址，不要带 /openapi/v1 前缀",
                },
            },
        }],
        "tags": [
            {"name": TAG_PAIRING, "description": "申请一次性绑定码，等用户显性授权后拿到 device_id"},
            {"name": TAG_DEVICE, "description": "确认屏幕尺寸、色彩与能力位，决定卡片怎么排"},
            {"name": TAG_CONTENT, "description": "把算好的内容推成一张 e-ink 卡片"},
            {"name": TAG_EVENT, "description": "回收显示结果，用于重试与告警"},
        ],
        "security": [{"AppKey": []}],
        "paths": {
            "/openapi/v1/pairing/start": {
                "post": {
                    "tags": [TAG_PAIRING],
                    "summary": "申请一次性绑定码",
                    "operationId": "startPairing",
                    "description": (
                        "把返回的 `display_code` 展示给用户（如「K7M-2QP」），让他在小悟后台『授权管理』里输入并点「允许」。\n\n"
                        "码默认 10 分钟有效、只能用一次。授权成功后轮询 `GET /openapi/v1/pairing/{code}` 拿 `device_id`。\n\n"
                        "```bash\ncurl -X POST \"$BASE/openapi/v1/pairing/start\" \\\n"
                        "  -H \"X-App-Key: $APP_TOKEN\" -H \"Content-Type: application/json\" \\\n"
                        "  -d '{\"label\":\"Codex 脉动\",\"scopes\":[\"device.content.write\"]}'\n```"
                    ),
                    "requestBody": {
                        "required": True,
                        "content": {"application/json": {
                            "schema": {"$ref": "#/components/schemas/PairingStartRequest"},
                            "example": {"label": "Codex 脉动", "scopes": ["device.content.write"]},
                        }},
                    },
                    "responses": {
                        "201": _json("绑定码已创建", {"$ref": "#/components/schemas/PairingStartResponse"}, {
                            "code": "K7M-2QP", "display_code": "K7M-2QP", "expires_in": 600,
                            "expires_at": "2026-11-02T12:00:00+00:00",
                            "confirm_path": "/pair?code=K7M-2QP",
                            "confirm_hint": "请让用户登录小悟后台并打开『授权管理』，输入上方绑定码确认",
                            "label": "Codex 脉动", "scopes": ["device.content.write"],
                        }),
                        "401": _err("App Token 无效", "invalid_app_key", "invalid app key"),
                        "403": _err("缺少 scope", "missing_scope", "missing scope: device.content.write"),
                    },
                },
            },
            "/openapi/v1/pairing/{code}": {
                "get": {
                    "tags": [TAG_PAIRING],
                    "summary": "轮询绑定结果",
                    "operationId": "getPairingStatus",
                    "description": (
                        "建议每 3 秒轮一次，拿到 `approved / denied / expired` 任一终态就停。\n"
                        "只有 `approved` 才返回 `device_id`，也只有此时才能推送。\n\n"
                        "```bash\ncurl \"$BASE/openapi/v1/pairing/K7M-2QP\" -H \"X-App-Key: $APP_TOKEN\"\n```"
                    ),
                    "parameters": [{
                        "name": "code", "in": "path", "required": True,
                        "description": "绑定码，必须是本应用申请的（跨应用查询返回 404）",
                        "schema": {"type": "string"},
                        "example": "K7M-2QP",
                    }],
                    "responses": {
                        "200": _json("当前状态", {"$ref": "#/components/schemas/PairingStatus"}, {
                            "code": "K7M-2QP", "status": "approved", "device_id": "DC:DA:0C:44:6D:D4",
                            "grant_id": "grt_7f2c1a", "expires_at": "2026-11-02T12:00:00+00:00",
                        }),
                        "404": _err("码不存在或不属于该应用", "pairing_code_not_found", "pairing code not found"),
                    },
                },
            },
            "/openapi/v1/devices/{device_id}": {
                "get": {
                    "tags": [TAG_DEVICE],
                    "summary": "查询设备能力",
                    "operationId": "getDevice",
                    "description": (
                        "确认这块屏的尺寸、色彩与能力位，据此决定卡片放几条、能不能用红色强调。\n"
                        "需要 `device.read` scope。\n\n"
                        "```bash\ncurl \"$BASE/openapi/v1/devices/DC:DA:0C:44:6D:D4\" -H \"X-App-Key: $APP_TOKEN\"\n```"
                    ),
                    "parameters": [{
                        "name": "device_id", "in": "path", "required": True,
                        "description": "确权成功后拿到的 device_id（就是设备 MAC）",
                        "schema": {"type": "string"}, "example": "DC:DA:0C:44:6D:D4",
                    }],
                    "responses": {
                        "200": _json("设备能力", {"$ref": "#/components/schemas/DeviceResponse"}, {
                            "device_id": "DC:DA:0C:44:6D:D4",
                            "capabilities": {"screen_a": {"w": 400, "h": 300, "colors": 2},
                                             "screen_b": {"w": 400, "h": 300, "colors": 4}},
                        }),
                        "404": _err("设备不存在", "device_not_found", "device not found"),
                    },
                },
            },
            "/openapi/v1/devices/{device_id}/content": {
                "post": {
                    "tags": [TAG_CONTENT],
                    "summary": "推送 e-ink 卡片",
                    "operationId": "pushContent",
                    "description": (
                        "异步接口：返回 `202` 只代表已入队，设备下次唤醒才会拉取并显示。\n\n"
                        "**两个前置条件**（缺一就失败）：\n"
                        "1. 该设备已被用户授权给你的应用，否则 `403 pairing_required`；\n"
                        "2. `template` 必须是平台已注册且启用的模板，`data` 要满足该模板的必填字段，否则 `400 / 422`。\n\n"
                        "当前已注册模板：`notice-card`（title, content）、`daily-schedule`（title, items）、"
                        "`homework-list`、`vocabulary-card`、`care-reminder`。\n\n"
                        "```bash\ncurl -X POST \"$BASE/openapi/v1/devices/DC:DA:0C:44:6D:D4/content\" \\\n"
                        "  -H \"X-App-Key: $APP_TOKEN\" -H \"Content-Type: application/json\" \\\n"
                        "  -d '{\"template\":\"notice-card\",\"screen\":\"A\",\"data\":{\"title\":\"API 额度\",\"content\":\"剩余 $42.10 · 6 天后重置\"},\"expires_in_seconds\":900}'\n```"
                    ),
                    "parameters": [{
                        "name": "device_id", "in": "path", "required": True,
                        "schema": {"type": "string"}, "example": "DC:DA:0C:44:6D:D4",
                    }],
                    "requestBody": {
                        "required": True,
                        "content": {"application/json": {
                            "schema": {"$ref": "#/components/schemas/ContentPushRequest"},
                            "example": {
                                "template": "notice-card",
                                "screen": "A",
                                "data": {"title": "API 额度", "content": "剩余 $42.10 · 6 天后重置"},
                                "expires_in_seconds": 900,
                            },
                        }},
                    },
                    "responses": {
                        "202": _json("已入队", {"$ref": "#/components/schemas/PushAccepted"}, {
                            "task_id": "9f8c2b1a4d5e6f708192a3b4", "status": "queued",
                        }),
                        "400": _err("模板不存在或已停用", "unknown_template", "unknown or disabled template"),
                        "403": _err("设备未授权给本应用（需先确权）", "pairing_required",
                                    "该设备尚未授权给此应用。请先调用 POST /openapi/v1/pairing/start"),
                        "404": _err("设备不存在", "device_not_found", "device not found"),
                        "422": _err("data 缺少模板必填字段", "template_validation_failed",
                                    "template_validation_failed: missing title"),
                    },
                },
            },
            "/openapi/v1/devices/{device_id}/events": {
                "get": {
                    "tags": [TAG_EVENT],
                    "summary": "拉取设备事件回执",
                    "operationId": "listDeviceEvents",
                    "description": (
                        "用于确认内容是否真的显示、失败原因是什么。需要 `device.event.read` scope。\n"
                        "生产环境更推荐配置 webhook（`device.event` / `task.displayed` / `task.failed`）避免轮询。\n\n"
                        "```bash\ncurl \"$BASE/openapi/v1/devices/DC:DA:0C:44:6D:D4/events\" -H \"X-App-Key: $APP_TOKEN\"\n```"
                    ),
                    "parameters": [{
                        "name": "device_id", "in": "path", "required": True,
                        "schema": {"type": "string"}, "example": "DC:DA:0C:44:6D:D4",
                    }],
                    "responses": {
                        "200": _json("事件列表（按时间倒序）", {
                            "type": "array", "items": {"$ref": "#/components/schemas/DeviceEvent"},
                        }, [{
                            "event_type": "task.displayed", "data": {"task_id": "9f8c2b1a4d5e6f708192a3b4"},
                            "occurred_at": "2026-11-02T03:20:11+00:00", "received_at": "2026-11-02T03:20:19+00:00",
                        }]),
                    },
                },
            },
        },
        "components": {
            "securitySchemes": {
                "AppKey": {
                    "type": "apiKey", "in": "header", "name": "X-App-Key",
                    "description": "开发者后台「接入引导 → 签发凭证」得到的 App Token，只显示一次，请存进钥匙串。",
                },
            },
            "schemas": {
                "PairingStartRequest": {
                    "type": "object",
                    "properties": {
                        "label": {"type": "string", "maxLength": 40,
                                  "description": "给用户看的应用名，会出现在授权列表里"},
                        "scopes": {
                            "type": "array", "items": {"type": "string",
                                                       "enum": list(_SCOPE_NOTE)},
                            "default": ["device.content.write"],
                            "description": "申请的权限，默认只申请写入内容",
                        },
                    },
                    "example": {"label": "Codex 脉动", "scopes": ["device.content.write"]},
                },
                "PairingStartResponse": {
                    "type": "object",
                    "required": ["code", "display_code", "expires_in", "expires_at"],
                    "properties": {
                        "code": {"type": "string", "description": "绑定码（轮询时用）"},
                        "display_code": {"type": "string", "description": "展示给用户的码，可能带分隔符"},
                        "expires_in": {"type": "integer", "description": "有效秒数，默认 600"},
                        "expires_at": {"type": "string", "format": "date-time"},
                        "confirm_path": {"type": "string", "description": "授权页相对路径，可拼成链接/二维码给用户"},
                        "confirm_hint": {"type": "string"},
                        "label": {"type": "string"},
                        "scopes": {"type": "array", "items": {"type": "string"}},
                    },
                },
                "PairingStatus": {
                    "type": "object",
                    "required": ["code", "status"],
                    "properties": {
                        "code": {"type": "string"},
                        "status": {"type": "string", "enum": ["pending", "approved", "denied", "expired"]},
                        "device_id": {"type": "string", "nullable": True,
                                      "description": "仅 status=approved 时返回"},
                        "grant_id": {"type": "string", "nullable": True},
                        "expires_at": {"type": "string", "format": "date-time"},
                    },
                },
                "DeviceResponse": {
                    "type": "object",
                    "required": ["device_id", "capabilities"],
                    "properties": {
                        "device_id": {"type": "string"},
                        "capabilities": {"type": "object", "additionalProperties": True,
                                         "description": "设备上报的能力位：屏幕尺寸、色彩数、固件版本等"},
                    },
                },
                "ContentPushRequest": {
                    "type": "object",
                    "required": ["template", "data"],
                    "properties": {
                        "template": {"type": "string", "pattern": "^[a-z0-9-]+$",
                                     "description": "已注册的模板 ID，如 notice-card / daily-schedule"},
                        "screen": {"type": "string", "enum": ["A", "B", "both"], "default": "A",
                                   "description": "A 屏黑白（此刻事实），B 屏四色（趋势/预警）"},
                        "data": {"type": "object", "additionalProperties": True,
                                 "description": "模板字段，须满足该模板 required"},
                        "expires_in_seconds": {"type": "integer", "minimum": 30, "maximum": 604800,
                                               "default": 3600, "description": "内容有效期，过期设备不再展示"},
                    },
                    "example": {"template": "notice-card", "screen": "A",
                                "data": {"title": "API 额度", "content": "剩余 $42.10 · 6 天后重置"},
                                "expires_in_seconds": 900},
                },
                "PushAccepted": {
                    "type": "object",
                    "required": ["task_id", "status"],
                    "properties": {
                        "task_id": {"type": "string"},
                        "status": {"type": "string", "enum": ["queued"]},
                    },
                },
                "DeviceEvent": {
                    "type": "object",
                    "properties": {
                        "event_type": {"type": "string",
                                       "description": "如 task.displayed / task.failed / device.event"},
                        "data": {"type": "object", "additionalProperties": True},
                        "occurred_at": {"type": "string", "format": "date-time"},
                        "received_at": {"type": "string", "format": "date-time"},
                    },
                },
                "Error": {
                    "type": "object",
                    "required": ["error"],
                    "properties": {
                        "error": {"type": "string", "description": "机器可判定的错误码"},
                        "message": {"type": "string", "description": "给人看的说明"},
                    },
                    "example": {"error": "pairing_required", "message": "该设备尚未授权给此应用"},
                },
            },
        },
        "x-scopes": _SCOPE_NOTE,
        "x-flow": [
            {"step": 1, "name": "签发凭证", "detail": "后台签发 App Token（X-App-Key）"},
            {"step": 2, "name": "确权绑定", "detail": "pairing/start → 用户确认 → pairing/{code} 拿 device_id"},
            {"step": 3, "name": "交付数据", "detail": "静态页 / 你的 HTTP 端点，二选一"},
            {"step": 4, "name": "推送上线", "detail": "POST /content，202 入队，events 或 webhook 回收执"},
        ],
    }


def to_markdown(spec: dict[str, Any] | None = None) -> str:
    """把规范渲染成一份可直接下载/贴给 LLM 的 Markdown 速查。"""
    spec = spec or build_spec()
    info = spec["info"]
    lines: list[str] = []
    add = lines.append

    add(f"# {info['title']}")
    add("")
    add(f"> 版本 {info['version']} · OpenAPI 3.0.3 · 机器可读版：`/openapi/isv.json`（或 `.yaml`）")
    add("")
    add(info["description"].strip())
    add("")
    add("## 基础信息")
    add("")
    add("| 项 | 值 |")
    add("| --- | --- |")
    add("| Base URL | `http://127.0.0.1:8104`（沙箱；生产地址由平台下发） |")
    add("| 鉴权 | 请求头 `X-App-Key: <App Token>` |")
    add("| 编码 | `Content-Type: application/json`，UTF-8 |")
    add("| 错误体 | `{\"error\": \"code\", \"message\": \"人话\"}` |")
    add("")
    add("## 权限（scopes）")
    add("")
    add("| scope | 含义 |")
    add("| --- | --- |")
    for scope, note in _SCOPE_NOTE.items():
        add(f"| `{scope}` | {note} |")
    add("")
    add("## 接入四步")
    add("")
    add("| 步 | 名称 | 做什么 |")
    add("| --- | --- | --- |")
    for item in spec.get("x-flow", []):
        add(f"| {item['step']} | {item['name']} | {item['detail']} |")
    add("")
    add("## 接口清单")
    add("")
    add("| 方法 | 路径 | 说明 |")
    add("| --- | --- | --- |")
    for path, ops in spec["paths"].items():
        for method, op in ops.items():
            add(f"| `{method.upper()}` | `{path}` | {op.get('summary', '')} |")
    add("")

    for path, ops in spec["paths"].items():
        for method, op in ops.items():
            add(f"### {method.upper()} {path}")
            add("")
            add(f"**{op.get('summary', '')}** · `operationId: {op.get('operationId', '')}`")
            add("")
            if op.get("description"):
                add(op["description"].strip())
                add("")
            params = op.get("parameters") or []
            if params:
                add("| 参数 | 位置 | 必填 | 类型 | 说明 |")
                add("| --- | --- | --- | --- | --- |")
                for p in params:
                    add(f"| `{p['name']}` | {p['in']} | {'是' if p.get('required') else '否'} | "
                        f"{(p.get('schema') or {}).get('type', 'string')} | {p.get('description', '')} |")
                add("")
            req = op.get("requestBody", {}).get("content", {}).get("application/json")
            if req:
                add("请求体：")
                add("")
                add("```json")
                add(_dump(req.get("example", {})))
                add("```")
                add("")
            if op.get("responses"):
                add("响应：")
                add("")
                for code, resp in op["responses"].items():
                    add(f"- `{code}` {resp.get('description', '')}")
                    body = (resp.get("content") or {}).get("application/json") or {}
                    if body.get("example") is not None:
                        add("")
                        add("```json")
                        add(_dump(body["example"]))
                        add("```")
                        add("")
                add("")

    add("## 数据结构")
    add("")
    for name, schema in spec["components"]["schemas"].items():
        add(f"### {name}")
        add("")
        if schema.get("type") == "object" and schema.get("properties"):
            add("| 字段 | 类型 | 必填 | 说明 |")
            add("| --- | --- | --- | --- |")
            required = set(schema.get("required") or [])
            for field, prop in schema["properties"].items():
                ptype = prop.get("type", "object")
                if ptype == "array":
                    ptype = f"array<{(prop.get('items') or {}).get('type', 'object')}>"
                add(f"| `{field}` | {ptype} | {'是' if field in required else '否'} | "
                    f"{prop.get('description', '')} |")
        elif schema.get("type") == "array":
            add("数组，元素见 `DeviceEvent`。")
        add("")
    return "\n".join(lines).rstrip() + "\n"


def _dump(value: Any) -> str:
    import json

    return json.dumps(value, ensure_ascii=False, indent=2)
