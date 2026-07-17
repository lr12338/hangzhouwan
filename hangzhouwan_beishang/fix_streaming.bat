@echo off
chcp 65001 >nul
title 流媒体问题修复工具

echo ========================================
echo 流媒体问题修复工具
echo ========================================
echo.

echo [1/5] 清理残留进程...
taskkill /F /IM ffmpeg.exe 2>nul
taskkill /F /IM nginx.exe 2>nul
echo 进程清理完成

echo.
echo [2/5] 检查Python环境...
python --version >nul 2>&1
if errorlevel 1 (
    echo 错误: 未找到Python环境
    pause
    exit /b 1
)
echo Python环境正常

echo.
echo [3/5] 运行RTMP连接测试...
python test_rtmp_connection.py

echo.
echo [4/5] 运行系统修复...
python fix_streaming_issues.py

echo.
echo [5/5] 生成启动脚本...
if exist start_system.bat (
    echo 启动脚本已就绪
) else (
    echo 警告: 启动脚本未生成
)

echo.
echo ========================================
echo 修复完成！
echo ========================================
echo.
echo 下一步操作：
echo 1. 查看 fix_streaming.log 了解详细修复结果
echo 2. 查看 rtmp_test_report.txt 了解连接测试结果
echo 3. 使用 start_system.bat 启动优化系统
echo.
pause

