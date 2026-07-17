import cv2
import threading
import subprocess
import time
import logging

import utils_demo.config
from utils_demo.method import get_bbox_center, should_draw_bbox
from utils_demo import getais
from utils_demo.plot import Plot_one_box
from utils_demo.method import get_bbox_center, should_draw_bbox
from utils_demo.find_ship import haversine,beishang_predict_longitude_latitude,beixia_predict_longitude_latitude, find_nearest_ship

class StreamConfig:
    def __init__(self, stream_id, stream_url, rtmp_url,
                 forbidden_rectangles, forbidden_polygons,
                 predict_coord_func, **kwargs):
        self.stream_id = stream_id
        self.stream_url = stream_url
        self.rtmp_url = rtmp_url
        self.forbidden_rectangles = forbidden_rectangles
        self.forbidden_polygons = forbidden_polygons
        self.predict_coord_func = predict_coord_func
        self.extra_params = kwargs


class VideoStreamHandler:
    def __init__(self, detector, config):
        self.detector = detector
        self.config = config
        self.frames = []
        self.frame_lock = threading.Lock()
        self.running = False
        self.ffmpeg_process = None

    def start(self):
        self.running = True
        self.read_thread = threading.Thread(target=self.read_video_stream, daemon=True)
        self.read_thread.start()

        self.process_thread = threading.Thread(target=self.detect_and_stream, daemon=True)
        self.process_thread.start()

    def stop(self):
        self.running = False
        if self.ffmpeg_process:
            self.ffmpeg_process.stdin.close()
            self.ffmpeg_process.wait()

    def start_ffmpeg_stream(self):
        """初始化FFmpeg推流进程"""
        ffmpeg_path = utils_demo.config.ffmpeg_path
        command = [
            ffmpeg_path,
            '-re',  # 按实际帧率读取
            '-loglevel', 'error',  # 错误日志输出 quiet 完全禁用
            '-rw_timeout', '5000000',  # 5秒超时
            '-analyzeduration', '1M',
            '-probesize', '1M',
            '-y',
            '-f', 'rawvideo',
            '-vcodec', 'rawvideo',
            '-pix_fmt', 'bgr24',
            '-s', '1280x960',  # 根据输出分辨率调整
            '-r', '15',
            '-i', '-',
            '-c:v', 'libx264',
            '-preset', 'veryfast',
            '-tune', 'zerolatency',
            '-g', '10',
            '-crf', '30',
            '-f', 'flv',
            self.config.rtmp_url
        ]
        return subprocess.Popen(command, stdin=subprocess.PIPE)

    def read_video_stream(self):
        """读取视频流（带自动重连）"""
        reconnect_interval = 5
        while self.running:
            cap = None
            try:
                cap = cv2.VideoCapture(self.config.stream_url)
                if not cap.isOpened():
                    raise ConnectionError(f"无法连接视频流 {self.config.stream_id}")

                logging.info(f"视频流 {self.config.stream_id} 连接成功")
                consecutive_failures = 0

                while self.running:
                    ret, frame = cap.read()
                    if not ret:
                        consecutive_failures += 1
                        if consecutive_failures >= 3:
                            logging.error(f"视频流 {self.config.stream_id} 连续读取失败，重新连接...")
                            break
                        time.sleep(1)
                        continue

                    consecutive_failures = 0
                    with self.frame_lock:
                        self.frames = [frame]  # 仅保留最新帧

                    time.sleep(0.03)  # 控制帧率

            except Exception as e:
                logging.error(f"视频流 {self.config.stream_id} 异常: {str(e)}")
            finally:
                if cap:
                    cap.release()
                logging.info(f"{reconnect_interval}秒后重连视频流 {self.config.stream_id}...")
                time.sleep(reconnect_interval)

    def detect_and_stream(self):
        """执行目标检测并推流"""
        self.ffmpeg_process = self.start_ffmpeg_stream()

        max_retries = 3  # 最大重试次数
        retry_delay = 5  # 重试间隔（秒）

        while self.running:
            # 初始化/重启FFmpeg进程
            if not self.ensure_ffmpeg_alive():
                time.sleep(retry_delay)
                continue

            # 获取最新帧
            frame = None
            with self.frame_lock:
                if self.frames:
                    frame = self.frames[0]

            if frame is None:
                time.sleep(0.01)
                continue

            # 使用当前流的配置进行处理
            boxes, scores, class_ids = self.detector.detect(frame)

            processed_frame = frame.copy()

            matched_mmsi = set()
            for box, score, class_id in zip(boxes, scores, class_ids):
                # print(box, score, class_id)
                x, y, w, h = box.astype(int)
                x1, y1, x2, y2 = x, y, x + w, y + h
                center = get_bbox_center(x1, y1, x2, y2)

                # 使用当前流的配置检查是否绘制
                if should_draw_bbox(
                        center,
                        self.config.forbidden_rectangles,
                        self.config.forbidden_polygons
                ):
                # if True:
                    # 使用当前流的坐标转换函数
                    lon1, lat1 = self.config.predict_coord_func(x1, y1, x2, y2)
                    # 查找最近的船舶（可扩展流特定逻辑）
                    nearest_ship = find_nearest_ship(
                        lon1, lat1,
                        getais.ship_data_dict,
                        matched_mmsi
                    )

                    if nearest_ship:
                        ship_info = f"{nearest_ship['ShipName']}, MMSI:{nearest_ship['MMSI']}, 速度:{nearest_ship['Speed']}节"
                        label= ship_info
                        matched_mmsi.add(f"{nearest_ship['MMSI']}")
                        processed_frame = Plot_one_box(
                            [x1, y1, x2, y2], processed_frame, label,
                            color=(0, 255, 0), line_thickness=2,
                            font_path= utils_demo.config.font_path, font_size=20)
                    else:
                        label = "Unkown Ship"
                        processed_frame = Plot_one_box(
                            [x1, y1, x2, y2], processed_frame, label,
                            color=(0, 255, 0), line_thickness=2,
                            font_path= utils_demo.config.font_path, font_size=20)
                    # print(label)
                        # 推流处理
            try:
                resized_frame = cv2.resize(processed_frame, (1280, 960))
                self.ffmpeg_process.stdin.write(resized_frame.tobytes())
                failed_attempts = 0  # 重置失败计数器
                # 本地显示（可选）
                cv2.imshow(f'Stream {self.config.stream_id}', resized_frame)
                cv2.waitKey(1)

            except (BrokenPipeError, IOError, AttributeError) as e:
                logging.error(f"推流失败: {str(e)}")
                self.safe_terminate_ffmpeg()
                failed_attempts += 1

                if failed_attempts >= max_retries:
                    logging.error(f"连续失败超过{max_retries}次，暂停推流")
                    self.safe_terminate_ffmpeg()
                    time.sleep(retry_delay)
                continue

            except Exception as e:
                logging.error(f"未知推流错误: {str(e)}")
                self.safe_terminate_ffmpeg()
                continue



    def ensure_ffmpeg_alive(self):
        """确保FFmpeg进程正常运行"""
        if self.ffmpeg_process and self.ffmpeg_process.poll() is None:
            return True

        logging.info("正在启动FFmpeg推流进程...")
        self.safe_terminate_ffmpeg()  # 清理旧进程

        try:
            self.ffmpeg_process = self.start_ffmpeg_stream()
            time.sleep(2)  # 等待进程初始化
            if self.ffmpeg_process.poll() is not None:
                raise RuntimeError("FFmpeg进程启动后立即退出")
            return True
        except Exception as e:
            logging.error(f"FFmpeg启动失败: {str(e)}")
            self.safe_terminate_ffmpeg()
            return False

    def safe_terminate_ffmpeg(self):
        """安全终止FFmpeg进程"""
        if self.ffmpeg_process:
            try:
                self.ffmpeg_process.stdin.close()
            except:
                pass
            try:
                self.ffmpeg_process.terminate()
            except:
                pass
            try:
                self.ffmpeg_process.wait(timeout=5)
            except:
                pass
        self.ffmpeg_process = None

