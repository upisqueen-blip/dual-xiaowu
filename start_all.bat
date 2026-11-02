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

echo Step 2 of 3: Starting content console on port 8094
start "Content Console 8094" /min /D "%ROOT%\apps\content-console-cn" cmd /k "set INKSIGHT_BACKEND_API_BASE=%BACKEND_URL%&& npm run dev"

echo Step 3 of 3: Starting platform console on port 8101
start "PC Console 8101" /min /D "%ROOT%\apps\pc-console" cmd /k "set XIAOWU_API_BASE=%BACKEND_URL%&& npm run dev"

echo.
echo  All services started. Three minimized windows are on the taskbar.
echo    Backend API:        http://localhost:8104
echo    Content console:    http://localhost:8094
echo    Platform console:   http://localhost:8101
echo      Developer center: http://localhost:8101/developer
echo      Content config:   http://localhost:8101/content-config
echo.
echo  Tip: open 8094 or 8101 in your browser after a few seconds.
echo  To stop: close the corresponding command windows.
echo.
pause
goto :eof

:free_port
for /f "tokens=5" %%P in ('netstat -ano ^| findstr ":%1 "') do taskkill /F /PID %%P >nul 2>&1
goto :eof
