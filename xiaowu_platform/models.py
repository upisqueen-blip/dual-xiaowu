from typing import Any, Literal

from pydantic import BaseModel, Field


class ScreenCapability(BaseModel):
    id: Literal["A", "B"]
    width: int = 400
    height: int = 300
    colors: list[str] = Field(default_factory=lambda: ["black", "white"])


class DeviceCapabilities(BaseModel):
    model: Literal["xiaowu-s3-dual"] = "xiaowu-s3-dual"
    screens: list[ScreenCapability] = Field(default_factory=lambda: [ScreenCapability(id="A"), ScreenCapability(id="B")])
    buttons: list[str] = Field(default_factory=lambda: ["left", "right", "confirm"])
    audio_input: bool = True
    audio_output: bool = True
    storage: bool = True
    wifi: bool = True


class DeviceRegister(BaseModel):
    device_id: str = Field(min_length=3, max_length=80, pattern=r"^[A-Za-z0-9:_-]+$")
    model: Literal["xiaowu-s3-dual"] = "xiaowu-s3-dual"
    firmware_version: str = Field(default="simulator", max_length=40)


class Heartbeat(BaseModel):
    firmware_version: str = Field(max_length=40)
    battery_voltage: float | None = Field(default=None, ge=0, le=6)
    rssi: int | None = Field(default=None, ge=-127, le=0)
    runtime_mode: Literal["active", "interval"] = "active"


class ContentPush(BaseModel):
    template: str = Field(min_length=1, max_length=80, pattern=r"^[a-z0-9-]+$")
    screen: Literal["A", "B", "both"] = "A"
    data: dict[str, Any]
    expires_in_seconds: int = Field(default=3600, ge=30, le=604800)


class TaskAck(BaseModel):
    status: Literal["displayed", "failed", "dismissed"]
    detail: str | None = Field(default=None, max_length=500)


class MobileContentPush(ContentPush):
    title: str = Field(default="", max_length=80)


class TemplateUpsert(BaseModel):
    template_id: str = Field(min_length=2, max_length=80, pattern=r"^[a-z0-9-]+$")
    name: str = Field(min_length=1, max_length=80)
    category: Literal["education", "elder-care", "family", "office", "general"] = "general"
    schema_definition: dict[str, Any] = Field(default_factory=dict)
    enabled: bool = True


class PasswordLogin(BaseModel):
    username: str = Field(min_length=2, max_length=50)
    password: str = Field(min_length=4, max_length=128)


class ContentConfigIn(BaseModel):
    """双屏内容配置（dual_config）整体写入。

    结构与 dual 控制台 / 小程序共用，顶层字段：
    version / sceneKey / config(各场景 A/B 屏配置) / content(课程表·便签·作业·阅读…) /
    screen / deviceMac / savedAt。后台只负责持久化与下发，不解释内部结构。
    """

    payload: dict[str, Any]


class DeveloperAppCreate(BaseModel):
    name: str = Field(min_length=2, max_length=80)
    scopes: list[Literal["device.content.write", "device.read", "device.event.read", "webhook.manage"]]


class DeveloperRegister(BaseModel):
    name: str = Field(min_length=2, max_length=50)
    organization: str = Field(default="", max_length=100)
    role: Literal["developer", "content_creator", "ip_owner", "channel_partner", "other"] = "developer"
    email: str = Field(min_length=5, max_length=120, pattern=r"^[^\s@]+@[^\s@]+\.[^\s@]+$")
    password: str = Field(min_length=8, max_length=128)
    agreement_version: Literal["2026-10-23"]
    accepted_terms: bool
    accepted_content_policy: bool


class DeveloperLogin(BaseModel):
    email: str = Field(min_length=5, max_length=120)
    password: str = Field(min_length=8, max_length=128)


class AdminLogin(BaseModel):
    username: str = Field(min_length=2, max_length=50)
    password: str = Field(min_length=4, max_length=128)


class SandboxAppCreate(BaseModel):
    name: str = Field(min_length=2, max_length=80)
    scene: Literal["office", "study", "family", "finance", "wellness", "custom"]
    description: str = Field(default="", max_length=500)


class ModelConfigUpsert(BaseModel):
    provider: Literal["openai-compatible", "deepseek", "qwen", "doubao"]
    model: str = Field(min_length=1, max_length=100)
    base_url: str = Field(default="", max_length=500)
    api_key: str = Field(default="", max_length=500)
    system_prompt: str = Field(default="", max_length=2000)


class ReviewSubmit(BaseModel):
    version: str = Field(default="0.1.0", max_length=40)
    notes: str = Field(default="", max_length=1000)


class GithubImportIn(BaseModel):
    repo_url: str = Field(min_length=8, max_length=300)
    branch: str = Field(default="", max_length=80)
    entry: str = Field(default="", max_length=200)


class DataSourceIn(BaseModel):
    endpoint: str = Field(min_length=8, max_length=500)
    token: str = Field(default="", max_length=300)
    sample: dict[str, Any] | None = None


class ReviewDecision(BaseModel):
    decision: Literal["approve", "reject"]
    note: str = Field(default="", max_length=1000)


class WebhookCreate(BaseModel):
    url: str = Field(min_length=10, max_length=500, pattern=r"^https://")
    events: list[Literal["device.event", "task.displayed", "task.failed"]]


class PairingStartIn(BaseModel):
    """ISV 申请一次性绑定码：label 会显示在用户的授权列表里，必须写真实设备名。"""

    label: str = Field(default="", max_length=40)
    scopes: list[str] = Field(default_factory=lambda: ["device.content.write"])


class PairingConfirmIn(BaseModel):
    """用户确认授权时选定的设备（必须是该用户已绑定的设备）。"""

    device_id: str = Field(min_length=4, max_length=64)


class DeviceEvent(BaseModel):
    event_type: Literal["button.pressed", "content.confirmed", "content.dismissed", "voice.intent"]
    data: dict[str, Any] = Field(default_factory=dict)
    occurred_at: str | None = None
