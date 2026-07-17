import logging
import time
import threading
from detector import YOLOv7
from stream_handler import VideoStreamHandler, StreamConfig
from utils_demo.config import STREAM_CONFIGS
from utils_demo import getais


def configure_logging():
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s - %(levelname)s - %(message)s",
        handlers=[logging.StreamHandler()]
    )


def main():
    configure_logging()

    # 初始化检测器
    detector = YOLOv7('./weights/best.onnx', conf_thres=0.1, iou_thres=0.1)

    # 创建流处理器
    stream_handlers = []
    for config_name, config_data in STREAM_CONFIGS.items():
        config = StreamConfig(**config_data)
        handler = VideoStreamHandler(detector, config)
        stream_handlers.append(handler)

    # 启动AIS数据更新
    ais_thread = threading.Thread(target=getais.main, daemon=True)
    ais_thread.start()

    # 启动所有流处理
    for handler in stream_handlers:
        handler.start()

    # 主循环管理
    try:
        while True:
            time.sleep(3600)  # 每小时检查一次
    except KeyboardInterrupt:
        logging.info("正在关闭...")
    finally:
        for handler in stream_handlers:
            handler.stop()
        logging.info("所有流处理已停止")


if __name__ == "__main__":
    main()