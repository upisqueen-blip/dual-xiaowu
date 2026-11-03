# 森穹小悟 S3 开放平台

面向森穹小悟 ESP32-S3 墨水屏终端的开源业务平台。行业开发者通过 OpenAPI、Webhook、内容模板和插件接入教育、养老、家庭、办公等业务；硬件设计、板级支持包（BSP）、屏幕驱动、生产与校准资料不在本仓库开放。

## 当前范围

- 仅支持 `xiaowu-s3-dual`：ESP32-S3、A/B 双 400×300 墨水屏
- 能力：双屏、按键、麦克风、扬声器、本地存储、Wi-Fi
- 提供：设备注册、心跳、能力查询、内容任务下发、任务确认、插件清单、S3 模拟器
- 开放接入：Developer App、Bearer Token、最小作用域、模板校验、设备事件、Webhook Outbox
- 管理端：微信小程序负责用户内容管理；PC 端负责完整运营管理
- 暂不提供：C3/NFC/并口屏适配、真实 BSP、OTA 签名服务、医疗诊断功能

## 已迁移的完整 S3 产品

- `apps/content-console-cn/`：原 `dual/` 完整国内 PC 双屏后台
- `apps/miniapp-cn/`：原 `miniprogram-xiaowu/` 完整微信小程序
- `firmware/s3/`：原 `firmware-dual/` 完整 S3 双屏固件业务源码
- `services/legacy-api/`：原后台兼容 API，保证上述界面功能可继续联调
- `apps/content-console-cn/`：唯一在用的 PC 双屏内容后台（旧 fork 已删除，需要时从 git 历史 `7f7d386` 取回）
- `apps/pc-console/`：仅运营人员使用的 PC 运营后台

迁移详情与端口规划见 [docs/full-migration.md](docs/full-migration.md)。

## 五分钟启动

需要 Python 3.11+：

> **一键启动（推荐，Windows）**：直接双击 `run_backend.bat`，会自动建虚拟环境、装依赖并启动后端。
> 后端默认端口 **8104**（与 `apps/content-console-cn` 的前端默认后端地址一致）。

```bash
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe -m uvicorn INKSIGHT_platform.main:app --reload --port 8104
```

以上命令不依赖系统 PATH 中存在 `pip` 或 `uvicorn`。如果 PowerShell 允许激活脚本，也可以先执行 `.\.venv\Scripts\Activate.ps1`，之后再使用 `python -m pip` 和 `python -m uvicorn`。

启动后：
- 接口文档（Swagger）：`http://127.0.0.1:8104/docs`
- 健康检查：`GET http://127.0.0.1:8104/health`
- 管理员账号通过环境变量 `INKSIGHT_ADMIN_USERNAME` 和 `INKSIGHT_ADMIN_PASSWORD` 配置；请在本机设置，不要提交真实凭据。

另开终端运行模拟器：

```bash
python tools/s3_simulator.py --server http://127.0.0.1:8104
```

模拟器首次启动会在开发模式自动注册，随后轮询任务。打开 `http://127.0.0.1:8104/docs` 可调用 OpenAPI；管理员登录账号由环境变量提供。生产环境必须设置 `INKSIGHT_DEV_MODE=false` 并配置专用管理员账号、密码及管理密钥。

PowerShell 本地启动管理员示例（密码仅存在于当前终端会话）：

```powershell
$env:INKSIGHT_ADMIN_USERNAME = "your-admin-name"
$env:INKSIGHT_ADMIN_PASSWORD = Read-Host "管理员密码" -AsSecureString | ConvertFrom-SecureString -AsPlainText
python -m uvicorn INKSIGHT_platform.main:app --reload --port 8104
```

> **端口说明**：本平台后端统一运行在 **8104**。`apps/content-console-cn`（PC 内容后台）通过环境变量 `INKSIGHT_BACKEND_API_BASE` 默认指向 `http://127.0.0.1:8104`；`services/legacy-api` 为旧的兼容后端，已在本次整合中让出 8104，请勿同时占用。

### PC 运营后台

```bash
cd apps/pc-console
npm install
npm run dev
```

访问 `http://127.0.0.1:8101`。当前提供运营概览、全量设备、内容任务、模板、插件和审计日志页面。

### 微信小程序

使用微信开发者工具导入 `apps/miniapp/`。Demo 默认连接 `http://127.0.0.1:8100`；真机调试时需改成局域网或 HTTPS 地址。小程序仅提供用户设备选择、内容模板、A/B 屏推送和历史记录。

推送示例：

```bash
curl -X POST http://127.0.0.1:8100/openapi/v1/devices/SIM-S3-001/content \
  -H "X-App-Key: dev-app-key" -H "Content-Type: application/json" \
  -d '{"template":"notice-card","screen":"A","data":{"title":"康复训练","content":"15:30 完成抬腿训练 3 组"}}'
```

### 双屏内容配置（小程序与 PC 后台共用）

课程表、便签、今日作业、阅读任务等双屏内容配置由小程序与 PC 后台读写**同一份** `dual_config`，两端互不覆盖。配置按设备维度存储，设备下次唤醒即拉取渲染。

- PC 后台：进入运营后台后点击左侧「双屏内容配置」，选择设备后可编辑场景 / A·B 屏模块 / 课程表 / 便签 / 作业 / 阅读，并实时预览。
- 微信小程序：首页「双屏内容管理」进入，按设备编辑同一份数据。

接口（小程序走用户令牌，PC 走管理员密钥）：

```bash
# 读取（移动端，需 X-User-Token 且该设备已绑定用户）
GET  /api/v1/mobile/devices/{device_id}/content-config
# 保存（移动端）
POST /api/v1/mobile/devices/{device_id}/content-config
# 读取（PC 后台，需 X-Admin-Key）
GET  /admin/v1/devices/{device_id}/content-config
# 保存（PC 后台）
POST /admin/v1/devices/{device_id}/content-config
```

返回结构：`{ "config": <dual_config 整体>, "updated_at": "..." }`；空配置时 `config` 为 `null`。


## 仓库边界

| 内容 | 状态 |
|---|---|
| 后台、开放协议、插件运行时、模拟器、示例 | 开源 |
| S3 业务固件接口/SDK 头文件 | 计划开放 |
| PCB、原理图、BOM、Gerber、引脚表 | 闭源 |
| BSP 源码、屏幕波形、电源与量产校准 | 闭源 |
| 官方云、OTA 签名、企业设备运营 | 商业服务 |

详细设计见 [docs/architecture.md](docs/architecture.md)，端侧权限边界见 [docs/admin-surfaces.md](docs/admin-surfaces.md)，开发路线见 [ROADMAP.md](ROADMAP.md)。

开发者从 [十分钟快速开始](docs/01-quickstart/README.md) 进入；设备协议、OpenAPI、模板和插件规范分别位于 `docs/03-device-protocol`、`docs/04-openapi`、`docs/05-template-development`、`docs/06-plugin-development`。

## 安全提示

当前自动注册只用于本地开发。生产环境应由工厂为每台设备注入唯一凭据，并通过一次性激活完成换钥。禁止在固件、插件或仓库中保存平台主密钥。

## License

平台代码采用 Apache-2.0。`docs/hardware-boundary.md` 中列出的硬件与闭源资产不因本仓库许可而开放。
