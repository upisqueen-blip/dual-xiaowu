@echo off
REM ============================================================
REM  Local domain proxy launcher
REM  Serves http://dual.localhost and http://dualcn.localhost
REM  so that the local topology matches production (domain
REM  based, not port based). Backend stays hidden behind the
REM  Next.js rewrites, exactly like production.
REM  Keep this file ASCII-only (cmd.exe uses the OEM codepage).
REM ============================================================
setlocal
cd /d "%~dp0"

set "NODE_EXE=node"
where node >nul 2>&1
if errorlevel 1 (
    if exist "C:\Users\upisq\.workbuddy\binaries\node\versions\22.22.2-6\node.exe" (
        set "NODE_EXE=C:\Users\upisq\.workbuddy\binaries\node\versions\22.22.2-6\node.exe"
    ) else (
        echo [error] node.exe not found. Install Node.js or add it to PATH.
        pause
        exit /b 1
    )
)

netstat -ano | findstr /R /C:":80 .*LISTENING" >nul 2>&1
if not errorlevel 1 (
    echo [warn] Port 80 is already in use. Run stop.bat first, or start with another port:
    echo        set DEV_PROXY_PORT=8080 ^&^& start.bat
    echo.
    netstat -ano | findstr /R /C:":80 .*LISTENING"
    echo.
    pause
    exit /b 1
)

echo ============================================================
echo  Local domain proxy
echo ============================================================
echo   Portal / ops  : http://dual.localhost/
echo   Ops overview  : http://dual.localhost/center
echo   Content console: http://dualcn.localhost/
echo   Route table   : http://localhost/
echo.
echo   Backend 8104 is NOT exposed as a domain - it is reached
echo   through /backend and /api rewrites, same as production.
echo   Ctrl+C stops the proxy.
echo ============================================================
echo.

"%NODE_EXE%" server.js
echo.
echo [stopped] proxy exited.
pause
