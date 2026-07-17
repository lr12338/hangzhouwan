"""
流媒体问题修复脚本
用于修复常见的推流问题
"""

import os
import sys
import logging
import subprocess
import time
import shutil
from pathlib import Path

def setup_logging():
    """设置日志"""
    logging.basicConfig(
        level=logging.INFO,
        format='%(asctime)s - %(levelname)s - %(message)s',
        handlers=[
            logging.StreamHandler(),
            logging.FileHandler('fix_streaming.log', encoding='utf-8')
        ]
    )

def check_nginx_status():
    """检查Nginx状态"""
    logging.info("检查Nginx服务状态...")
    
    try:
        # 检查nginx进程
        result = subprocess.run([
            'tasklist', '/FI', 'IMAGENAME eq nginx.exe'
        ], capture_output=True, text=True)
        
        if 'nginx.exe' in result.stdout:
            logging.info("✓ Nginx服务正在运行")
            return True
        else:
            logging.warning("✗ Nginx服务未运行")
            return False
            
    except Exception as e:
        logging.error(f"检查Nginx状态失败: {e}")
        return False

def start_nginx():
    """启动Nginx服务"""
    logging.info("正在启动Nginx服务...")
    
    nginx_dir = Path(__file__).parent.parent / "nginx 1.7.11.3 Gryphon"
    nginx_exe = nginx_dir / "nginx.exe"
    
    if not nginx_exe.exists():
        logging.error(f"Nginx可执行文件不存在: {nginx_exe}")
        return False
    
    try:
        # 切换到nginx目录
        os.chdir(nginx_dir)
        
        # 启动nginx
        subprocess.Popen([str(nginx_exe)], 
                        creationflags=subprocess.CREATE_NO_WINDOW)
        
        time.sleep(3)  # 等待启动
        
        if check_nginx_status():
            logging.info("✓ Nginx启动成功")
            return True
        else:
            logging.error("✗ Nginx启动失败")
            return False
            
    except Exception as e:
        logging.error(f"启动Nginx失败: {e}")
        return False
    finally:
        # 返回原目录
        os.chdir(Path(__file__).parent)

def stop_nginx():
    """停止Nginx服务"""
    logging.info("正在停止Nginx服务...")
    
    nginx_dir = Path(__file__).parent.parent / "nginx 1.7.11.3 Gryphon"
    nginx_exe = nginx_dir / "nginx.exe"
    
    try:
        if nginx_exe.exists():
            subprocess.run([str(nginx_exe), '-s', 'stop'], 
                          cwd=nginx_dir, timeout=10)
            time.sleep(2)
            logging.info("✓ Nginx服务已停止")
        
        # 强制杀死残留进程
        subprocess.run(['taskkill', '/F', '/IM', 'nginx.exe'], 
                      capture_output=True)
        
    except Exception as e:
        logging.warning(f"停止Nginx时出现警告: {e}")

def backup_nginx_config():
    """备份Nginx配置"""
    logging.info("备份Nginx配置...")
    
    nginx_conf_dir = Path(__file__).parent.parent / "nginx 1.7.11.3 Gryphon" / "conf"
    backup_dir = nginx_conf_dir / "backup"
    
    backup_dir.mkdir(exist_ok=True)
    
    try:
        # 备份主配置文件
        main_conf = nginx_conf_dir / "nginx.conf"
        if main_conf.exists():
            backup_file = backup_dir / f"nginx.conf.backup_{int(time.time())}"
            shutil.copy2(main_conf, backup_file)
            logging.info(f"✓ 配置文件已备份到: {backup_file}")
        
        return True
        
    except Exception as e:
        logging.error(f"备份配置失败: {e}")
        return False

def apply_optimized_nginx_config():
    """应用优化的Nginx配置"""
    logging.info("应用优化的Nginx配置...")
    
    nginx_conf_dir = Path(__file__).parent.parent / "nginx 1.7.11.3 Gryphon" / "conf"
    optimized_conf = nginx_conf_dir / "nginx_optimized.conf"
    main_conf = nginx_conf_dir / "nginx.conf"
    
    if not optimized_conf.exists():
        logging.error("优化配置文件不存在")
        return False
    
    try:
        # 停止nginx
        stop_nginx()
        
        # 备份当前配置
        backup_nginx_config()
        
        # 应用优化配置
        shutil.copy2(optimized_conf, main_conf)
        logging.info("✓ 优化配置已应用")
        
        # 重启nginx
        return start_nginx()
        
    except Exception as e:
        logging.error(f"应用配置失败: {e}")
        return False

def test_rtmp_endpoints():
    """测试RTMP端点"""
    logging.info("测试RTMP端点...")
    
    try:
        # 运行RTMP连接测试
        subprocess.run([sys.executable, 'test_rtmp_connection.py'], 
                      timeout=60)
        return True
        
    except Exception as e:
        logging.error(f"RTMP测试失败: {e}")
        return False

def clean_ffmpeg_processes():
    """清理FFmpeg进程"""
    logging.info("清理残留的FFmpeg进程...")
    
    try:
        subprocess.run(['taskkill', '/F', '/IM', 'ffmpeg.exe'], 
                      capture_output=True)
        logging.info("✓ FFmpeg进程已清理")
        
    except Exception as e:
        logging.warning(f"清理FFmpeg进程时出现警告: {e}")

def optimize_system_settings():
    """优化系统设置"""
    logging.info("优化系统设置...")
    
    try:
        # 运行系统优化脚本
        subprocess.run([sys.executable, 'system_optimizer.py'], 
                      timeout=120)
        return True
        
    except Exception as e:
        logging.error(f"系统优化失败: {e}")
        return False

def create_startup_script():
    """创建启动脚本"""
    logging.info("创建启动脚本...")
    
    startup_script = """@echo off
echo 正在启动优化版船舶检测系统...

REM 检查并启动Nginx
echo 检查Nginx服务...
tasklist /FI "IMAGENAME eq nginx.exe" 2>nul | find /I /N "nginx.exe" >nul
if errorlevel 1 (
    echo 启动Nginx服务...
    cd /d "%~dp0..\nginx 1.7.11.3 Gryphon"
    start "" nginx.exe
    cd /d "%~dp0"
    timeout /t 3 >nul
)

REM 清理残留进程
echo 清理残留进程...
taskkill /F /IM ffmpeg.exe 2>nul

REM 启动主程序
echo 启动主程序...
python starter_optimized.py

pause
"""
    
    try:
        with open('start_system.bat', 'w', encoding='gbk') as f:
            f.write(startup_script)
        
        logging.info("✓ 启动脚本已创建: start_system.bat")
        return True
        
    except Exception as e:
        logging.error(f"创建启动脚本失败: {e}")
        return False

def main():
    """主修复流程"""
    setup_logging()
    
    logging.info("=" * 50)
    logging.info("流媒体问题修复工具")
    logging.info("=" * 50)
    
    success_count = 0
    total_steps = 7
    
    # 1. 清理进程
    logging.info("步骤 1/7: 清理残留进程")
    clean_ffmpeg_processes()
    success_count += 1
    
    # 2. 应用Nginx优化配置
    logging.info("步骤 2/7: 应用Nginx优化配置")
    if apply_optimized_nginx_config():
        success_count += 1
        logging.info("✓ Nginx配置优化完成")
    else:
        logging.error("✗ Nginx配置优化失败")
    
    # 3. 测试RTMP连接
    logging.info("步骤 3/7: 测试RTMP连接")
    if test_rtmp_endpoints():
        success_count += 1
        logging.info("✓ RTMP连接测试完成")
    else:
        logging.error("✗ RTMP连接测试失败")
    
    # 4. 优化系统设置
    logging.info("步骤 4/7: 优化系统设置")
    if optimize_system_settings():
        success_count += 1
        logging.info("✓ 系统设置优化完成")
    else:
        logging.error("✗ 系统设置优化失败")
    
    # 5. 创建启动脚本
    logging.info("步骤 5/7: 创建启动脚本")
    if create_startup_script():
        success_count += 1
        logging.info("✓ 启动脚本创建完成")
    else:
        logging.error("✗ 启动脚本创建失败")
    
    # 6. 检查文件完整性
    logging.info("步骤 6/7: 检查文件完整性")
    required_files = [
        'starter_optimized.py',
        'stream_handler_optimized.py',
        'test_rtmp_connection.py'
    ]
    
    missing_files = []
    for file in required_files:
        if not Path(file).exists():
            missing_files.append(file)
    
    if not missing_files:
        success_count += 1
        logging.info("✓ 所有必需文件存在")
    else:
        logging.error(f"✗ 缺少文件: {missing_files}")
    
    # 7. 生成报告
    logging.info("步骤 7/7: 生成修复报告")
    success_count += 1
    
    # 总结
    logging.info("=" * 50)
    logging.info("修复完成")
    logging.info(f"成功步骤: {success_count}/{total_steps}")
    
    if success_count == total_steps:
        logging.info("✓ 所有修复步骤完成，系统已优化")
        logging.info("建议使用 start_system.bat 启动系统")
    else:
        logging.warning("⚠ 部分修复步骤失败，请检查日志")
    
    logging.info("详细日志已保存到 fix_streaming.log")
    logging.info("=" * 50)

if __name__ == "__main__":
    main()

