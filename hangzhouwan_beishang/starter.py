import logging
import time
import threading
import os
import sys
import psutil

import utils_demo.config
from detector import YOLOv7
from stream_handler import VideoStreamHandler, StreamConfig
from config import STREAM_CONFIGS
from utils_demo import getais

# 全局控制变量
stop_event = threading.Event()
RUN_TIME = 3600  # 3小时（单位：秒）


def configure_logging():
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s - %(levelname)s - %(message)s",
        handlers=[logging.StreamHandler()]
    )


def cleanup_resources():
    """清理资源并停止所有线程"""
    current_pid = os.getpid()
    process = psutil.Process(current_pid)
    logging.info(f"Cleaning up resources for PID {current_pid}")

    # 停止所有流处理器
    for handler in stream_handlers:
        handler.stop()

    # 终止子进程
    for child in process.children(recursive=True):
        try:
            child.terminate()
        except psutil.NoSuchProcess:
            continue

    gone, alive = psutil.wait_procs(process.children(), timeout=5)
    for p in alive:
        try:
            p.kill()
        except psutil.NoSuchProcess:
            continue

    stop_event.set()
    logging.info("资源清理完成")


def restart_script():
    """重启当前脚本"""
    logging.info("准备重启脚本...")
    cleanup_resources()
    python = sys.executable
    os.execv(python, [python] + sys.argv)


def initialize_handlers(detector):
    """初始化流处理器"""
    handlers = []
    for config_name, config_data in STREAM_CONFIGS.items():
        config = StreamConfig(**config_data)
        handler = VideoStreamHandler(detector, config)
        handler.start()
        handlers.append(handler)
    return handlers


def main():
    configure_logging()

    # 初始化检测器
    detector = YOLOv7(utils_demo.config.detector_path, conf_thres=0.1, iou_thres=0.1)

    # 启动AIS线程
    ais_thread = threading.Thread(target=getais.main, daemon=True)
    ais_thread.start()

    # 主运行循环
    start_time = time.time()
    global stream_handlers
    stream_handlers = initialize_handlers(detector)

    try:
        while True:
            # 检查运行时间
            if time.time() - start_time > RUN_TIME:
                logging.info("达到运行时间限制，准备重启...")
                restart_script()

            # 监控线程状态
            time.sleep(60)  # 每分钟检查一次

    except KeyboardInterrupt:
        logging.info("接收到中断信号，正在关闭...")
    finally:
        cleanup_resources()
        logging.info("应用程序已完全关闭")


if __name__ == "__main__":
    main()