@echo off
REM ============================================================
REM  Stop whatever is listening on port 80 / 443 (the local
REM  domain proxy). Keep this file ASCII-only.
REM ============================================================
setlocal enabledelayedexpansion
set "FOUND="

for /f "tokens=5" %%P in ('netstat -ano ^| findstr /R /C:":80 .*LISTENING"') do (
    echo [stop] port 80  PID %%P
    taskkill /F /PID %%P >nul 2>&1
    set "FOUND=1"
)

for /f "tokens=5" %%P in ('netstat -ano ^| findstr /R /C:":443 .*LISTENING"') do (
    echo [stop] port 443 PID %%P
    taskkill /F /PID %%P >nul 2>&1
    set "FOUND=1"
)

if not defined FOUND echo [info] Nothing is listening on port 80 / 443.
echo.
echo Done.
