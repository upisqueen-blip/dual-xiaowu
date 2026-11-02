@echo off
REM Xiaowu S3 Open Platform - one-click backend launcher
REM Backend port 8104, matching apps/content-console-cn proxy default.
REM NOTE: keep this file ASCII-only. cmd.exe parses .bat with GBK codepage,
REM any Chinese here becomes mojibake and breaks execution.
setlocal
cd /d "%~dp0"

if not exist ".venv\Scripts\python.exe" (
  echo [setup] creating virtualenv .venv ...
  python -m venv .venv
  if errorlevel 1 goto :fail
)

REM A venv folder can exist while dependencies are missing (interrupted install,
REM venv created by an editor/IDE). Check imports, not just python.exe.
".venv\Scripts\python.exe" -c "import uvicorn, fastapi, pydantic, PIL, multipart, cryptography" >nul 2>&1
if errorlevel 1 (
  echo [setup] installing dependencies from requirements.txt ...
  ".venv\Scripts\python.exe" -m pip install --upgrade pip
  ".venv\Scripts\python.exe" -m pip install -r requirements.txt
  if errorlevel 1 goto :fail
  ".venv\Scripts\python.exe" -c "import uvicorn, fastapi, pydantic, PIL, multipart, cryptography" >nul 2>&1
  if errorlevel 1 goto :fail
)

if not exist ".env" (
  echo [warn] .env not found. Copy .env.example to .env and fill in the values.
)

echo.
echo ============================================================
echo  Backend:  http://0.0.0.0:8104   (LAN: http://192.168.x.x:8104)
echo  Swagger:  http://127.0.0.1:8104/docs
echo  Admin account: configure XIAOWU_ADMIN_USERNAME and XIAOWU_ADMIN_PASSWORD in .env
echo  Press Ctrl+C to stop
echo ============================================================
echo.
REM --host MUST be 0.0.0.0: 127.0.0.1:8104 is occupied by a hidden loopback
REM forward on this machine, and the firmware reaches the backend via LAN IP.
".venv\Scripts\python.exe" -m uvicorn xiaowu_platform.main:app --host 0.0.0.0 --port 8104 --reload
pause
exit /b 0

:fail
echo.
echo [error] environment setup failed. Fix pip/network or delete .venv and rerun.
pause
exit /b 1
