@echo off
REM ============================================================
REM  One-click: local DOMAIN mode
REM  dual.localhost / dualcn.localhost  ==  dual / dualcn online
REM
REM  Steps:
REM    1) switch-env.bat domain  (writes .env.local for both apps)
REM    2) restart the two Next dev servers (env.local is read at boot only)
REM    3) start backend 8104 if it is not running
REM    4) start the domain proxy on port 80
REM    5) open the browser
REM
REM  ASCII-only file: cmd.exe parses .bat with the OEM codepage.
REM ============================================================
setlocal
cd /d "%~dp0"
set "ROOT=%~dp0"
set "VENV=%ROOT%.venv\Scripts\python.exe"

echo ============================================================
echo  Local DOMAIN mode
echo ============================================================
echo.

echo [1/5] switching env to DOMAIN mode ...
call "%ROOT%switch-env.bat" domain >nul
if errorlevel 1 (
    echo [error] switch-env.bat failed.
    pause
    exit /b 1
)
echo       done: .env.local written for pc-console and content-console.

echo [2/5] restarting front-end dev servers (8094 / 8101) ...
call :free_port 8094
call :free_port 8101

echo [3/5] backend :8104 ...
set "BUSY8104="
for /f "tokens=5" %%P in ('netstat -ano ^| findstr /R /C:":8104 .*LISTENING"') do set "BUSY8104=1"
if defined BUSY8104 (
    echo       already running, left untouched.
) else (
    if not exist "%VENV%" (
        echo [error] .venv not found. Run run_backend.bat once to set it up.
        pause
        exit /b 1
    )
    start "XiaoWu Backend 8104" /min cmd /k "cd /d %ROOT% && %VENV% -m uvicorn xiaowu_platform.main:app --host 0.0.0.0 --port 8104 --reload"
    echo       started.
)

echo [4/5] starting dev servers and domain proxy ...
start "Content Console 8094" /min /D "%ROOT%apps\content-console-cn" cmd /k "npm run dev"
start "PC Console 8101" /min /D "%ROOT%apps\pc-console" cmd /k "npm run dev"

set "BUSY80="
for /f "tokens=5" %%P in ('netstat -ano ^| findstr /R /C:":80 .*LISTENING"') do set "BUSY80=1"
if defined BUSY80 (
    echo       port 80 already in use - proxy NOT started.
    echo       If that is not dev-proxy, run dev-proxy\stop.bat first.
) else (
    start "dev-proxy :80" /min cmd /k "cd /d %ROOT%dev-proxy && node server.js"
    echo       proxy started.
)

echo [5/5] waiting for the dev servers to compile ...
ping -n 10 127.0.0.1 >nul

echo.
echo ============================================================
echo   Portal       : http://dual.localhost/
echo   Ops overview : http://dual.localhost/center
echo   Content      : http://dualcn.localhost/
echo   Route table  : http://localhost/
echo ============================================================
echo.
echo   Flasher page stays in port mode:
echo     start_web_flash.bat  -^>  http://localhost:8091/zh/flash
echo.
echo.
echo   Close the "dev-proxy :80" window to stop domain mode.
echo   Tasks are minimized on the taskbar.
echo.
start "" http://dual.localhost/
pause
exit /b 0

:free_port
for /f "tokens=5" %%P in ('netstat -ano ^| findstr /R /C:":%~1 .*LISTENING"') do (
    echo       killing PID %%P on port %~1
    taskkill /F /PID %%P >nul 2>&1
)
exit /b 0
