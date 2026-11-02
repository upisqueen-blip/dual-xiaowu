@echo off
REM ============================================================
REM  Xiaowu all-in-one launcher (ASCII version)
REM  Starts backend 8104, content-console 8094, pc-console 8101
REM  Double-click to run. Close service windows to stop.
REM ============================================================

set "ROOT=E:\electron\screen\inksight-main\inksight-main\xiaowu-s3-open-platform"
set "VENV=%ROOT%\.venv\Scripts\python.exe"
set "BACKEND_URL=http://127.0.0.1:8104"

echo ============================================================
echo  Xiaowu all-in-one launcher
echo ============================================================
echo.

call :free_port 8104
call :free_port 8094
call :free_port 8101
echo.

echo Step 1 of 3: Starting backend API on port 8104
start "XiaoWu Backend 8104" /min cmd /k "cd /d %ROOT% && %VENV% -m uvicorn xiaowu_platform.main:app --host 0.0.0.0 --port 8104 --reload"

REM 上游地址统一由各 app 自己的 .env 提供，这里不再 set，
REM 避免"启动脚本一套、.env 一套"两处配置打架。
REM 首次运行且没有 .env 时，从 .env.example 复制一份（.env 不进 git）。
if not exist "%ROOT%\apps\pc-console\.env" copy /Y "%ROOT%\apps\pc-console\.env.example" "%ROOT%\apps\pc-console\.env" >nul
if not exist "%ROOT%\apps\content-console-cn\.env" copy /Y "%ROOT%\apps\content-console-cn\.env.example" "%ROOT%\apps\content-console-cn\.env" >nul

echo Step 2 of 3: Starting content console on port 8094
start "Content Console 8094" /min /D "%ROOT%\apps\content-console-cn" cmd /k "npm run dev"

echo Step 3 of 3: Starting platform console on port 8101
start "PC Console 8101" /min /D "%ROOT%\apps\pc-console" cmd /k "npm run dev"

echo.
echo  All services started. Three minimized windows are on the taskbar.
echo    Backend API:        http://localhost:8104
echo    Content console:    http://localhost:8094  (also http://localhost:8101/admin)
echo    Platform console:   http://localhost:8101
echo      Creator portal:   http://localhost:8101/
echo      Ops overview:     http://localhost:8101/center
echo      Content console:  http://localhost:8101/admin
echo.
echo  Tip: open 8094 or 8101 in your browser after a few seconds.
echo  To stop: close the corresponding command windows.
echo.
pause
goto :eof

:free_port
for /f "tokens=5" %%P in ('netstat -ano ^| findstr ":%1 "') do taskkill /F /PID %%P >nul 2>&1
goto :eof
