#!/usr/bin/env bash
# Xiaowu S3 Open Platform - Linux backend launcher (port 8104)
# 替代 Windows 专用的 run_backend.bat；在 Linux 服务器上使用。
set -euo pipefail
cd "$(dirname "$0")"

# 若服务器使用虚拟环境则取消下一行注释（或把 python 换成 .venv/bin/python）
# shellcheck disable=SC1091
# [ -f ".venv/bin/activate" ] && source .venv/bin/activate

# 环境变量优先级：系统环境变量 > 项目根目录 .env（由 settings.py 通过 python-dotenv 加载）
# 生产环境推荐把 XIAOWU_ADMIN_USERNAME / XIAOWU_ADMIN_PASSWORD 等通过 systemd Environment= 注入，
# 这样无需把含密钥的 .env 提交到 git。
HOST="${XIAOWU_HOST:-0.0.0.0}"
PORT="${XIAOWU_PORT:-8104}"

echo "Backend : http://${HOST}:${PORT}"
echo "Swagger : http://127.0.0.1:${PORT}/docs"
echo "Press Ctrl+C to stop"

exec python -m uvicorn xiaowu_platform.main:app --host "$HOST" --port "$PORT" ${XIAOWU_RELOAD:+--reload}
