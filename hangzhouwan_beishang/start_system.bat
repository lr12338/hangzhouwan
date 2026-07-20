@echo off
echo ���������������������ϵͳ...

REM ����1����鲢����Nginx����������������
echo [1/3] ���Nginx����...
tasklist /FI "IMAGENAME eq nginx.exe" 2>nul | find /I /N "nginx.exe" >nul
if errorlevel 1 (
    echo Nginxδ���У���������...
    cd /d "%~dp0..\nginx 1.7.11.3 Gryphon"
    start "" nginx.exe
    cd /d "%~dp0"
    timeout /t 3 >nul
    echo Nginx������
) else (
    echo Nginx��������
)

REM ����2����������FFmpeg����
echo [2/3] ������������...
taskkill /F /IM ffmpeg.exe 2>nul

REM ����3������������starter.py��
echo [3/3] ����������...
python starter.py

pause
