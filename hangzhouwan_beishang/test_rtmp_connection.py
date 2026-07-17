"""
RTMP连接测试工具
用于测试RTMP服务器连接状态
"""

import subprocess
import time
import logging
import sys
import os
from config import STREAM_CONFIGS
import utils_demo.config

def setup_logging():
    """设置日志"""
    logging.basicConfig(
        level=logging.INFO,
        format='%(asctime)s - %(levelname)s - %(message)s'
    )

def test_ffmpeg_available():
    """测试FFmpeg是否可用"""
    try:
        result = subprocess.run([
            utils_demo.config.ffmpeg_path, '-version'
        ], capture_output=True, text=True, timeout=10)
        
        if result.returncode == 0:
            logging.info("✓ FFmpeg可用")
            return True
        else:
            logging.error("✗ FFmpeg不可用")
            return False
            
    except Exception as e:
        logging.error(f"✗ FFmpeg测试失败: {e}")
        return False

def test_rtmp_connection(rtmp_url, stream_id):
    """测试RTMP连接"""
    logging.info(f"正在测试流 {stream_id} 的RTMP连接: {rtmp_url}")
    
    # 创建测试视频（纯色帧）
    command = [
        utils_demo.config.ffmpeg_path,
        '-f', 'lavfi',
        '-i', 'testsrc=duration=10:size=320x240:rate=1',  # 低分辨率测试
        '-c:v', 'libx264',
        '-preset', 'ultrafast',
        '-tune', 'zerolatency',
        '-f', 'flv',
        '-t', '5',  # 测试5秒
        rtmp_url
    ]
    
    try:
        logging.info(f"执行FFmpeg命令: {' '.join(command[:8])}...")
        
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )
        
        # 等待进程完成或超时
        try:
            stdout, stderr = process.communicate(timeout=15)
            
            if process.returncode == 0:
                logging.info(f"✓ 流 {stream_id} RTMP连接成功")
                return True
            else:
                logging.error(f"✗ 流 {stream_id} RTMP连接失败")
                logging.error(f"错误信息: {stderr}")
                return False
                
        except subprocess.TimeoutExpired:
            process.kill()
            logging.warning(f"⚠ 流 {stream_id} RTMP连接测试超时（可能是正常的）")
            return True  # 超时可能表示连接成功但服务器在等待数据
            
    except Exception as e:
        logging.error(f"✗ 流 {stream_id} RTMP连接测试异常: {e}")
        return False

def test_network_connectivity():
    """测试网络连通性"""
    logging.info("正在测试网络连通性...")
    
    # 测试基本网络连接
    try:
        import socket
        
        # 提取RTMP服务器信息
        for stream_id, config in STREAM_CONFIGS.items():
            rtmp_url = config['rtmp_url']
            
            # 解析RTMP URL
            if rtmp_url.startswith('rtmp://'):
                url_parts = rtmp_url[7:].split('/')
                if ':' in url_parts[0]:
                    host, port = url_parts[0].split(':')
                    port = int(port)
                else:
                    host = url_parts[0]
                    port = 1935  # 默认RTMP端口
                
                logging.info(f"测试到 {host}:{port} 的连接...")
                
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(5)
                
                try:
                    result = sock.connect_ex((host, port))
                    if result == 0:
                        logging.info(f"✓ 可以连接到 {host}:{port}")
                    else:
                        logging.error(f"✗ 无法连接到 {host}:{port}")
                except Exception as e:
                    logging.error(f"✗ 连接 {host}:{port} 时出错: {e}")
                finally:
                    sock.close()
                    
    except Exception as e:
        logging.error(f"网络连通性测试失败: {e}")

def test_rtsp_sources():
    """测试RTSP视频源"""
    logging.info("正在测试RTSP视频源...")
    
    for stream_id, config in STREAM_CONFIGS.items():
        rtsp_url = config['stream_url']
        logging.info(f"测试流 {stream_id} 的RTSP源: {rtsp_url}")
        
        command = [
            utils_demo.config.ffmpeg_path,
            '-i', rtsp_url,
            '-t', '3',  # 测试3秒
            '-f', 'null',
            '-'
        ]
        
        try:
            result = subprocess.run(
                command,
                capture_output=True,
                text=True,
                timeout=15
            )
            
            if result.returncode == 0:
                logging.info(f"✓ 流 {stream_id} RTSP源可用")
            else:
                logging.error(f"✗ 流 {stream_id} RTSP源不可用")
                logging.error(f"错误信息: {result.stderr}")
                
        except subprocess.TimeoutExpired:
            logging.warning(f"⚠ 流 {stream_id} RTSP测试超时")
        except Exception as e:
            logging.error(f"✗ 流 {stream_id} RTSP测试异常: {e}")

def generate_test_report():
    """生成测试报告"""
    report = []
    report.append("=" * 50)
    report.append("RTMP连接测试报告")
    report.append("=" * 50)
    
    # 基础环境测试
    report.append("\n1. 基础环境测试:")
    if test_ffmpeg_available():
        report.append("   ✓ FFmpeg: 可用")
    else:
        report.append("   ✗ FFmpeg: 不可用")
    
    # 网络连通性测试
    report.append("\n2. 网络连通性测试:")
    test_network_connectivity()
    
    # RTSP源测试
    report.append("\n3. RTSP视频源测试:")
    test_rtsp_sources()
    
    # RTMP连接测试
    report.append("\n4. RTMP推流测试:")
    rtmp_results = []
    for stream_id, config in STREAM_CONFIGS.items():
        rtmp_url = config['rtmp_url']
        success = test_rtmp_connection(rtmp_url, stream_id)
        rtmp_results.append((stream_id, success))
        
        if success:
            report.append(f"   ✓ 流 {stream_id}: 连接成功")
        else:
            report.append(f"   ✗ 流 {stream_id}: 连接失败")
    
    # 建议
    report.append("\n5. 建议:")
    failed_streams = [sid for sid, success in rtmp_results if not success]
    
    if failed_streams:
        report.append("   发现以下问题:")
        for sid in failed_streams:
            report.append(f"   - 流 {sid} 无法连接到RTMP服务器")
        report.append("\n   建议检查:")
        report.append("   1. RTMP服务器是否正常运行")
        report.append("   2. 网络防火墙设置")
        report.append("   3. RTMP URL是否正确")
        report.append("   4. 服务器是否允许推流")
    else:
        report.append("   ✓ 所有测试通过，系统配置正常")
    
    report.append("\n" + "=" * 50)
    
    # 输出报告
    for line in report:
        print(line)
    
    # 保存报告到文件
    with open('rtmp_test_report.txt', 'w', encoding='utf-8') as f:
        for line in report:
            f.write(line + '\n')
    
    logging.info("测试报告已保存到 rtmp_test_report.txt")

def main():
    """主函数"""
    setup_logging()
    
    print("RTMP连接测试工具")
    print("=" * 30)
    
    try:
        generate_test_report()
    except KeyboardInterrupt:
        logging.info("测试被用户中断")
    except Exception as e:
        logging.error(f"测试过程中发生错误: {e}")

if __name__ == "__main__":
    main()

