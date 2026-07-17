import logging
import time
import threading
import os
import sys
import psutil
import gc
from concurrent.futures import ThreadPoolExecutor

import utils_demo.config
from detector import YOLOv7
from stream_handler_optimized import OptimizedVideoStreamHandler, StreamConfig
from config import STREAM_CONFIGS
from utils_demo import getais

# 全局控制变量
stop_event = threading.Event()
RUN_TIME = 3600  # 3小时（单位：秒）

# 性能监控
performance_stats = {
    'memory_usage': [],
    'cpu_usage': [],
    'stream_fps': {}
}

def configure_logging():
    """配置日志系统"""
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
        handlers=[
            logging.StreamHandler(),
            logging.FileHandler('logs/stream_processing.log', encoding='utf-8')
        ]
    )
    
    # 设置第三方库日志级别
    logging.getLogger('cv2').setLevel(logging.WARNING)
    logging.getLogger('numpy').setLevel(logging.WARNING)

def optimize_system_settings():
    """优化系统设置"""
    try:
        import ctypes
        # Windows下设置进程优先级
        if sys.platform == 'win32':
            ctypes.windll.kernel32.SetPriorityClass(
                ctypes.windll.kernel32.GetCurrentProcess(), 0x80  # HIGH_PRIORITY_CLASS
            )
            logging.info("设置高优先级成功")
    except Exception as e:
        logging.warning(f"设置进程优先级失败: {e}")
    
    # 设置OpenCV线程数
    import cv2
    cv2.setNumThreads(2)
    cv2.setUseOptimized(True)
    
    # 禁用OpenCV的多线程（避免冲突）
    os.environ['OMP_NUM_THREADS'] = '1'

def monitor_performance():
    """性能监控线程"""
    process = psutil.Process()
    
    while not stop_event.is_set():
        try:
            # 内存使用率
            memory_info = process.memory_info()
            memory_mb = memory_info.rss / 1024 / 1024
            
            # CPU使用率
            cpu_percent = process.cpu_percent()
            
            performance_stats['memory_usage'].append(memory_mb)
            performance_stats['cpu_usage'].append(cpu_percent)
            
            # 保持最近100个数据点
            if len(performance_stats['memory_usage']) > 100:
                performance_stats['memory_usage'] = performance_stats['memory_usage'][-100:]
                performance_stats['cpu_usage'] = performance_stats['cpu_usage'][-100:]
            
            # 每分钟输出一次性能统计
            if len(performance_stats['memory_usage']) % 12 == 0:  # 5秒间隔，12次=1分钟
                avg_memory = sum(performance_stats['memory_usage'][-12:]) / 12
                avg_cpu = sum(performance_stats['cpu_usage'][-12:]) / 12
                
                logging.info(f"性能统计 - 内存: {avg_memory:.1f}MB, CPU: {avg_cpu:.1f}%")
                
                # 内存清理
                if avg_memory > 1000:  # 超过1GB时进行垃圾回收
                    gc.collect()
                    logging.info("执行内存清理")
            
        except Exception as e:
            logging.error(f"性能监控错误: {e}")
        
        time.sleep(5)

def cleanup_resources():
    """清理资源并停止所有线程"""
    current_pid = os.getpid()
    process = psutil.Process(current_pid)
    logging.info(f"正在清理资源 PID {current_pid}")

    # 停止所有流处理器
    for handler in stream_handlers:
        try:
            handler.stop()
        except Exception as e:
            logging.error(f"停止流处理器失败: {e}")

    # 终止子进程
    for child in process.children(recursive=True):
        try:
            child.terminate()
        except psutil.NoSuchProcess:
            continue

    # 等待进程结束
    gone, alive = psutil.wait_procs(process.children(), timeout=10)
    for p in alive:
        try:
            p.kill()
        except psutil.NoSuchProcess:
            continue

    stop_event.set()
    
    # 强制垃圾回收
    gc.collect()
    
    logging.info("资源清理完成")

def restart_script():
    """重启当前脚本"""
    logging.info("准备重启脚本...")
    cleanup_resources()
    time.sleep(2)  # 等待清理完成
    
    python = sys.executable
    os.execv(python, [python] + sys.argv)

def initialize_handlers(detector):
    """初始化流处理器"""
    handlers = []
    
    # 使用线程池来并行初始化
    with ThreadPoolExecutor(max_workers=2) as executor:
        futures = []
        
        for config_name, config_data in STREAM_CONFIGS.items():
            config = StreamConfig(**config_data)
            future = executor.submit(create_handler, detector, config)
            futures.append(future)
        
        # 等待所有处理器创建完成
        for future in futures:
            try:
                handler = future.result(timeout=30)
                if handler:
                    handlers.append(handler)
                    logging.info(f"流处理器 {handler.config.stream_id} 初始化成功")
            except Exception as e:
                logging.error(f"流处理器初始化失败: {e}")
    
    return handlers

def create_handler(detector, config):
    """创建单个流处理器"""
    try:
        handler = OptimizedVideoStreamHandler(detector, config)
        handler.start()
        return handler
    except Exception as e:
        logging.error(f"创建流处理器失败 {config.stream_id}: {e}")
        return None

def health_check(handlers):
    """健康检查线程"""
    check_interval = 60  # 每分钟检查一次
    
    while not stop_event.is_set():
        try:
            # 检查流处理器状态
            for handler in handlers:
                if not handler.running:
                    logging.warning(f"检测到流处理器 {handler.config.stream_id} 已停止")
                    # 可以在这里添加重启逻辑
            
            # 检查系统资源
            memory_usage = psutil.virtual_memory().percent
            cpu_usage = psutil.cpu_percent(interval=1)
            
            if memory_usage > 90:
                logging.warning(f"系统内存使用率过高: {memory_usage}%")
            
            if cpu_usage > 95:
                logging.warning(f"系统CPU使用率过高: {cpu_usage}%")
            
        except Exception as e:
            logging.error(f"健康检查错误: {e}")
        
        time.sleep(check_interval)

def main():
    # 确保日志目录存在
    os.makedirs('logs', exist_ok=True)
    
    configure_logging()
    optimize_system_settings()
    
    logging.info("=" * 50)
    logging.info("启动优化版船舶检测与跟踪系统")
    logging.info("=" * 50)

    try:
        # 初始化检测器
        logging.info("正在初始化YOLOv7检测器...")
        detector = YOLOv7(
            utils_demo.config.detector_path, 
            conf_thres=0.1, 
            iou_thres=0.1
        )
        logging.info("检测器初始化完成")

        # 启动AIS线程
        logging.info("启动AIS数据线程...")
        ais_thread = threading.Thread(target=getais.main, daemon=True)
        ais_thread.start()

        # 启动性能监控线程
        monitor_thread = threading.Thread(target=monitor_performance, daemon=True)
        monitor_thread.start()
        
        # 初始化流处理器
        logging.info("正在初始化流处理器...")
        global stream_handlers
        stream_handlers = initialize_handlers(detector)
        
        if not stream_handlers:
            raise RuntimeError("没有成功初始化任何流处理器")
        
        logging.info(f"成功初始化 {len(stream_handlers)} 个流处理器")

        # 启动健康检查线程
        health_thread = threading.Thread(target=health_check, args=(stream_handlers,), daemon=True)
        health_thread.start()

        # 主运行循环
        start_time = time.time()
        
        while True:
            current_time = time.time()
            
            # 检查运行时间
            if current_time - start_time > RUN_TIME:
                logging.info("达到运行时间限制，准备重启...")
                restart_script()

            # 定期输出运行状态
            if int(current_time) % 300 == 0:  # 每5分钟
                uptime = current_time - start_time
                logging.info(f"系统运行时间: {uptime/3600:.1f} 小时")

            time.sleep(60)  # 每分钟检查一次

    except KeyboardInterrupt:
        logging.info("接收到中断信号，正在关闭...")
    except Exception as e:
        logging.error(f"系统运行错误: {e}", exc_info=True)
        # 等待一段时间后重启
        time.sleep(10)
        restart_script()
    finally:
        cleanup_resources()
        logging.info("应用程序已完全关闭")

if __name__ == "__main__":
    main()
