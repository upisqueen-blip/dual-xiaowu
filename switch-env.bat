@echo off
setlocal
if "%~1"=="" goto usage
if /i "%~1"=="local" (
    > apps\content-console-cn\.env.local (
        echo INKSIGHT_BACKEND_API_BASE=http://127.0.0.1:8092
        echo NEXT_PUBLIC_BASE_PATH=
        echo NEXT_PUBLIC_DEVELOPER_CENTER_URL=/
    )
    echo.
    echo [本地模式] 前端 .env.local 已指向本地后端 http://127.0.0.1:8092
    echo   1^) 启动本地后端：双击 run-backend-local.bat （监听 0.0.0.0:8092^)
    echo   2^) 重启前端：  cd apps/content-console-cn ^&^& npm run dev
    echo   提示：本地后端数据库与线上独立，device 连本地需先在本地注册该设备。
    goto done
)
if /i "%~1"=="online" (
    > apps\content-console-cn\.env.local (
        echo INKSIGHT_BACKEND_API_BASE=https://dual.xiaowuai.vip/backend
        echo NEXT_PUBLIC_BASE_PATH=
        echo NEXT_PUBLIC_DEVELOPER_CENTER_URL=/
    )
    echo.
    echo [线上模式] 前端 .env.local 已指向线上后端 https://dual.xiaowuai.vip/backend
    echo   重启前端即可：  cd apps/content-console-cn ^&^& npm run dev
    goto done
)
:usage
echo 用法：
echo   switch-env.bat local    前端连本地后端（127.0.0.1:8092，需先跑 run-backend-local.bat^)
echo   switch-env.bat online   前端连线上后端（dual.xiaowuai.vip，无需本地后端^)
echo.
echo 切换后请重启前端 dev 服务，使 .env.local 生效。
:done
