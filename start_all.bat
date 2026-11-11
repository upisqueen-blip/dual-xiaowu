@echo off
REM ============================================================
REM  Xiaowu all-in-one launcher (ASCII version)
REM  Starts backend 8104, content-console 8094, pc-console 8101,
REM  plus website + online flasher (web 8091 / api 8092)
REM  Double-click to run. Close service windows to stop.
REM
REM  Optional 1st word picks the DEVICE (firmware) backend:
REM     start_all.bat local     device -> this PC (needs reflash)
REM     start_all.bat online    device -> cloud
REM  With no argument the device setting is left untouched.
REM
REM  Optional 2nd word:
REM     start_all.bat local noflash   skip the website/flasher pair
REM ============================================================

set "ROOT=E:\electron\screen\inksight-main\inksight-main\xiaowu-s3-open-platform"
set "VENV=%ROOT%\.venv\Scripts\python.exe"
set "BACKEND_URL=http://127.0.0.1:8104"
set "DEVARG=%~1"
set "SKIPFLASH=%~2"

echo ============================================================
echo  Xiaowu all-in-one launcher
echo ============================================================
echo.

call :free_port 8104
call :free_port 8094
call :free_port 8101
echo.

echo Step 1 of 5: Starting backend API on port 8104
start "XiaoWu Backend 8104" /min cmd /k "cd /d %ROOT% && %VENV% -m uvicorn xiaowu_platform.main:app --host 0.0.0.0 --port 8104 --reload"

REM 上游地址统一由各 app 自己的 .env 提供，这里不再 set，
REM 避免"启动脚本一套、.env 一套"两处配置打架。
REM 首次运行且没有 .env 时，从 .env.example 复制一份（.env 不进 git）。
if not exist "%ROOT%\apps\pc-console\.env" copy /Y "%ROOT%\apps\pc-console\.env.example" "%ROOT%\apps\pc-console\.env" >nul
if not exist "%ROOT%\apps\content-console-cn\.env" copy /Y "%ROOT%\apps\content-console-cn\.env.example" "%ROOT%\apps\content-console-cn\.env" >nul

echo Step 2 of 5: Starting content console on port 8094
start "Content Console 8094" /min /D "%ROOT%\apps\content-console-cn" cmd /k "npm run dev"

echo Step 3 of 5: Starting platform console on port 8101
start "PC Console 8101" /min /D "%ROOT%\apps\pc-console" cmd /k "npm run dev"

if /i "%SKIPFLASH%"=="noflash" (
    echo Step 4 of 5: Website / online flasher SKIPPED ^(noflash^).
) else (
    echo Step 4 of 5: Starting website + online flasher ^(web 8091 / api 8092^)
    call "%ROOT%start_web_flash.bat" quiet
)

echo Step 5 of 5: Starting local domain proxy (port 80)
call :maybe_proxy

echo.
echo  All services started. Minimized windows are on the taskbar.
echo    Backend API:        http://localhost:8104
echo    Content console:    http://localhost:8094/admin  (root "/" auto-redirects to /admin)
echo                        via portal: http://localhost:8101/admin
echo    Platform console:   http://localhost:8101
echo      Creator portal:   http://localhost:8101/
echo      Ops overview:     http://localhost:8101/center
echo      Content console:  http://localhost:8101/admin
echo    Website / flasher:  http://localhost:8091/zh/flash   (English: /en/flash)
echo                        its API: http://127.0.0.1:8092
echo    Domain mode:        http://dual.localhost/         (portal / ops)
echo                        http://dualcn.localhost/        (content console)
echo.
echo  Tip: open 8094 or 8101 in your browser after a few seconds.
echo  To stop: close the corresponding command windows.
echo.
ping -n 4 127.0.0.1 >nul
echo  ------------------------------------------------------------
echo   DEVICE (firmware) backend
echo  ------------------------------------------------------------
if /i "%DEVARG%"=="local"  call "%ROOT%\device-env.bat" local
if /i "%DEVARG%"=="online" call "%ROOT%\device-env.bat" online
call "%ROOT%\device-env.bat" lan
echo.
echo  To switch the DEVICE to this PC:  device-env.bat local flash
echo  To switch it back to the cloud:   device-env.bat online flash
echo.
pause
goto :eof

:free_port
for /f "tokens=5" %%P in ('netstat -ano ^| findstr ":%1 "') do taskkill /F /PID %%P >nul 2>&1
goto :eof

:maybe_proxy
REM Domain mode (dual.localhost / dualcn.localhost) needs port 80.
REM Never kill an existing listener - it may be IIS / nginx the user
REM started on purpose.
netstat -ano | findstr /R /C:":80 .*LISTENING" >nul 2>&1
if not errorlevel 1 (
    echo       port 80 already in use - domain proxy NOT started.
    echo       If that is not dev-proxy, run dev-proxy\stop.bat first.
    goto :eof
)
start "dev-proxy :80" /min cmd /k "cd /d %ROOT%\dev-proxy && node server.js"
echo       started: http://dual.localhost/  http://dualcn.localhost/
goto :eof
