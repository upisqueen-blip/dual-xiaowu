@echo off
cd /d %~dp0
echo 启动本地后端（xiaowu_platform）监听 0.0.0.0:8092 ...
echo 数据库：当前目录 data/xiaowu.db（与线上独立）
echo 读取仓库根 .env 中的 XIAOWU_* 配置（管理员账号 / 数据库路径等）
echo.
REM 优先激活项目已有的虚拟环境（按常见命名尝试）
if exist venv\Scripts\activate.bat call venv\Scripts\activate.bat
if exist .venv\Scripts\activate.bat call .venv\Scripts\activate.bat
if exist backend-venv\Scripts\activate.bat call backend-venv\Scripts\activate.bat
uvicorn xiaowu_platform.main:app --host 0.0.0.0 --port 8092 --reload
pause
