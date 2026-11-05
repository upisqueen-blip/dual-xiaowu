@echo off
REM ============================================================
REM  Optional fallback: add *.localhost entries to the hosts file.
REM  Chrome / Edge / Firefox already resolve any *.localhost to
REM  127.0.0.1, so you normally do NOT need this. Run it only if
REM  the domains do not open in your browser.
REM  Right-click -> Run as administrator. ASCII-only file.
REM ============================================================
setlocal
set "HOSTS=%SystemRoot%\System32\drivers\etc\hosts"

net session >nul 2>&1
if errorlevel 1 (
    echo [error] Administrator rights required. Right-click this file
    echo         and choose "Run as administrator".
    pause
    exit /b 1
)

findstr /I /C:"dual.localhost" "%HOSTS%" >nul 2>&1
if not errorlevel 1 (
    echo [info] hosts already contains dual.localhost / dualcn.localhost.
    pause
    exit /b 0
)

echo.>>"%HOSTS%"
echo # Xiaowu local domain proxy (added by dev-proxy\add-hosts.bat)>>"%HOSTS%"
echo 127.0.0.1 dual.localhost>>"%HOSTS%"
echo 127.0.0.1 dualcn.localhost>>"%HOSTS%"
echo [ok] Added:
echo      127.0.0.1 dual.localhost
echo      127.0.0.1 dualcn.localhost
echo.
echo If the browser still shows the old behaviour, run: ipconfig /flushdns
pause
