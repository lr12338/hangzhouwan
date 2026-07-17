"""
系统优化脚本
用于优化Windows系统设置以提高视频流处理性能
"""

import os
import sys
import subprocess
import logging
import winreg
import ctypes
from ctypes import wintypes

def setup_logging():
    """设置日志"""
    logging.basicConfig(
        level=logging.INFO,
        format='%(asctime)s - %(levelname)s - %(message)s'
    )

def is_admin():
    """检查是否有管理员权限"""
    try:
        return ctypes.windll.shell32.IsUserAnAdmin()
    except:
        return False

def run_as_admin():
    """以管理员权限运行"""
    if is_admin():
        return True
    else:
        logging.warning("需要管理员权限来应用某些优化设置")
        # 尝试重新以管理员权限运行
        ctypes.windll.shell32.ShellExecuteW(
            None, "runas", sys.executable, " ".join(sys.argv), None, 1
        )
        return False

def optimize_network_settings():
    """优化网络设置"""
    logging.info("正在优化网络设置...")
    
    try:
        # 优化TCP窗口大小
        subprocess.run([
            'netsh', 'int', 'tcp', 'set', 'global', 
            'autotuninglevel=normal'
        ], check=True)
        
        # 禁用Nagle算法（减少延迟）
        subprocess.run([
            'netsh', 'int', 'tcp', 'set', 'global', 
            'chimney=enabled'
        ], check=True)
        
        # 设置TCP参数
        subprocess.run([
            'netsh', 'int', 'tcp', 'set', 'global', 
            'rss=enabled'
        ], check=True)
        
        logging.info("网络设置优化完成")
        
    except subprocess.CalledProcessError as e:
        logging.error(f"网络设置优化失败: {e}")
    except Exception as e:
        logging.error(f"网络优化发生错误: {e}")

def optimize_registry_settings():
    """优化注册表设置"""
    logging.info("正在优化注册表设置...")
    
    optimizations = [
        # TCP/IP优化
        {
            'key': r'HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters',
            'values': {
                'TcpWindowSize': (winreg.REG_DWORD, 65536),
                'Tcp1323Opts': (winreg.REG_DWORD, 3),
                'DefaultTTL': (winreg.REG_DWORD, 64),
                'EnablePMTUDiscovery': (winreg.REG_DWORD, 1),
                'EnablePMTUBHDetect': (winreg.REG_DWORD, 0),
                'SackOpts': (winreg.REG_DWORD, 1),
                'MaxFreeTcbs': (winreg.REG_DWORD, 65536),
                'MaxHashTableSize': (winreg.REG_DWORD, 65536)
            }
        },
        # 多媒体优化
        {
            'key': r'HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile',
            'values': {
                'SystemResponsiveness': (winreg.REG_DWORD, 1),
                'NetworkThrottlingIndex': (winreg.REG_DWORD, 10)
            }
        }
    ]
    
    for opt in optimizations:
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, 
                              opt['key'].replace('HKEY_LOCAL_MACHINE\\', ''), 
                              0, winreg.KEY_ALL_ACCESS) as key:
                
                for name, (reg_type, value) in opt['values'].items():
                    winreg.SetValueEx(key, name, 0, reg_type, value)
                    logging.info(f"设置注册表项: {name} = {value}")
                    
        except PermissionError:
            logging.error(f"没有权限修改注册表项: {opt['key']}")
        except Exception as e:
            logging.error(f"修改注册表失败: {e}")

def optimize_power_settings():
    """优化电源设置"""
    logging.info("正在优化电源设置...")
    
    try:
        # 设置高性能电源计划
        subprocess.run([
            'powercfg', '/setactive', 'SCHEME_MIN'
        ], check=True)
        
        # 禁用USB选择性暂停
        subprocess.run([
            'powercfg', '/setacvalueindex', 'SCHEME_CURRENT', 
            'SUB_USB', 'USBSELECTIVESUSPEND', '0'
        ], check=True)
        
        # 禁用硬盘关闭
        subprocess.run([
            'powercfg', '/setacvalueindex', 'SCHEME_CURRENT', 
            'SUB_DISK', 'DISKIDLE', '0'
        ], check=True)
        
        # 应用设置
        subprocess.run(['powercfg', '/setactive', 'SCHEME_CURRENT'], check=True)
        
        logging.info("电源设置优化完成")
        
    except subprocess.CalledProcessError as e:
        logging.error(f"电源设置优化失败: {e}")

def set_process_priority():
    """设置进程优先级"""
    logging.info("正在设置进程优先级...")
    
    try:
        # 设置当前进程为高优先级
        import psutil
        current_process = psutil.Process()
        current_process.nice(psutil.HIGH_PRIORITY_CLASS)
        
        logging.info("进程优先级设置完成")
        
    except Exception as e:
        logging.error(f"设置进程优先级失败: {e}")

def optimize_windows_defender():
    """优化Windows Defender设置"""
    logging.info("正在优化Windows Defender设置...")
    
    try:
        # 添加项目目录到排除列表
        project_dir = os.path.dirname(os.path.abspath(__file__))
        
        subprocess.run([
            'powershell', '-Command',
            f'Add-MpPreference -ExclusionPath "{project_dir}"'
        ], check=True)
        
        logging.info(f"已将 {project_dir} 添加到Windows Defender排除列表")
        
    except subprocess.CalledProcessError as e:
        logging.error(f"Windows Defender优化失败: {e}")

def check_system_requirements():
    """检查系统需求"""
    logging.info("正在检查系统需求...")
    
    import psutil
    
    # 检查内存
    memory = psutil.virtual_memory()
    memory_gb = memory.total / (1024**3)
    
    if memory_gb < 8:
        logging.warning(f"系统内存较少: {memory_gb:.1f}GB, 建议至少8GB")
    else:
        logging.info(f"系统内存: {memory_gb:.1f}GB ✓")
    
    # 检查CPU
    cpu_count = psutil.cpu_count()
    if cpu_count < 4:
        logging.warning(f"CPU核心数较少: {cpu_count}, 建议至少4核")
    else:
        logging.info(f"CPU核心数: {cpu_count} ✓")
    
    # 检查磁盘空间
    disk = psutil.disk_usage('/')
    free_gb = disk.free / (1024**3)
    
    if free_gb < 10:
        logging.warning(f"磁盘空间不足: {free_gb:.1f}GB, 建议至少10GB")
    else:
        logging.info(f"可用磁盘空间: {free_gb:.1f}GB ✓")

def create_optimization_bat():
    """创建优化批处理文件"""
    bat_content = '''@echo off
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
'''
    
    with open('system_optimize.bat', 'w', encoding='gbk') as f:
        f.write(bat_content)
    
    logging.info("已创建系统优化批处理文件: system_optimize.bat")

def main():
    """主函数"""
    setup_logging()
    
    logging.info("=" * 50)
    logging.info("开始系统优化")
    logging.info("=" * 50)
    
    # 检查系统需求
    check_system_requirements()
    
    # 检查管理员权限
    if not is_admin():
        logging.warning("当前没有管理员权限，某些优化可能无法应用")
        logging.info("建议以管理员身份运行此脚本")
    
    try:
        # 应用优化
        optimize_network_settings()
        optimize_power_settings()
        set_process_priority()
        
        # 需要管理员权限的优化
        if is_admin():
            optimize_registry_settings()
            optimize_windows_defender()
        
        # 创建批处理文件
        create_optimization_bat()
        
        logging.info("=" * 50)
        logging.info("系统优化完成！")
        logging.info("建议重启计算机以应用所有设置")
        logging.info("=" * 50)
        
    except Exception as e:
        logging.error(f"系统优化过程中发生错误: {e}")

if __name__ == "__main__":
    main()

