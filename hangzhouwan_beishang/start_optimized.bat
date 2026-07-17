@echo off
chcp 65001 >nul
title 船舶检测与跟踪系统 - 优化版

echo ========================================
echo 船舶检测与跟踪系统 - 优化版启动脚本
echo ========================================
echo.

REM 检查Python环境
python --version >nul 2>&1
if errorlevel 1 (
    echo 错误: 未找到Python环境
    echo 请确保Python已正确安装并添加到PATH环境变量
    pause
    exit /b 1
)

REM 检查必要文件
if not exist "starter_optimized.py" (
    echo 错误: 未找到 starter_optimized.py 文件
    echo 请确保已正确部署优化文件
    pause
    exit /b 1
)

if not exist "stream_handler_optimized.py" (
    echo 错误: 未找到 stream_handler_optimized.py 文件
    echo 请确保已正确部署优化文件
    pause
    exit /b 1
)

REM 创建日志目录
if not exist "logs" mkdir logs

REM 检查Nginx服务
echo 正在检查Nginx服务状态...
tasklist /FI "IMAGENAME eq nginx.exe" 2>nul | find /I /N "nginx.exe" >nul
if errorlevel 1 (
    echo 警告: Nginx服务未运行
    echo 正在尝试启动Nginx...
    cd /d "%~dp0..\nginx 1.7.11.3 Gryphon"
    start "" nginx.exe
    cd /d "%~dp0"
    timeout /t 3 >nul
) else (
    echo Nginx服务运行正常
)

echo.
echo 系统检查完成，正在启动优化版程序...
echo.
echo 程序启动后将显示以下信息:
echo - 流处理器初始化状态
echo - 实时FPS统计
echo - 系统性能监控
echo - 错误和警告信息
echo.
echo 按 Ctrl+C 可安全停止程序
echo ========================================
echo.

REM 启动优化版程序
python starter_optimized.py

echo.
echo 程序已退出
pause

