@echo off
echo 正在应用系统优化设置...

REM 设置TCP参数
netsh int tcp set global autotuninglevel=normal
netsh int tcp set global chimney=enabled
netsh int tcp set global rss=enabled
netsh int tcp set global netdma=enabled

REM 设置高性能电源计划
powercfg /setactive SCHEME_MIN

REM 禁用不必要的服务
sc config "Windows Search" start= disabled
sc config "Superfetch" start= disabled

echo 优化完成！请重启计算机以应用所有设置。
pause
