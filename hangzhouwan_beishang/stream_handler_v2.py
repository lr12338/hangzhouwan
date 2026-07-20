import cv2
import threading
import subprocess
import time
import logging
import os
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

# 创建目标数据结构
class TrackedTarget:
    def __init__(self, box, label, mmsi):
        self.box = box          # [x1, y1, x2, y2]
        self.label = label      # 显示标签
        self.mmsi = mmsi        # 船舶唯一标识
        self.last_seen = 0      # 最后出现时间（帧计数）
        self.age = 0            # 目标存活时长
        self.smooth_box = box   # 平滑后的坐标

class VideoStreamHandler:
    def __init__(self, detector, config):
        self.detector = detector
        self.config = config
        self.frames = []
        self.frame_lock = threading.Lock()
        self.running = False
        self.ffmpeg_process = None
        self.target_cache = []  # 新增目标缓存
        self.cache_max_age = 50  # 最大保留帧数（约0.5秒@30fps）
        self.iou_threshold = 0.8  # 匹配阈值

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
        ffmpeg_path = os.environ.get("HANGZHOUWAN_FFMPEG_PATH", "ffmpeg")
        command = [
            ffmpeg_path,
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
        frame_counter = 0  # 全局帧计数器
        while self.running:
            # 获取最新帧
            frame = None
            with self.frame_lock:
                if self.frames:
                    frame = self.frames[0]

            if frame is None:
                time.sleep(0.01)
                continue
            # 执行检测
            current_detections = self.process_detections(frame)

            # 关联现有目标
            self.update_target_cache(current_detections, frame_counter)

            # 绘制所有有效目标
            processed_frame = self.draw_all_targets(frame.copy())

            # 推流处理...
            resized_frame = cv2.resize(processed_frame, (1280, 960))
            # cv2.imshow(f'Stream {self.config.stream_id}', resized_frame)
            # cv2.waitKey(1)
            self.ffmpeg_process.stdin.write(resized_frame.tobytes())

            frame_counter += 1

    def process_detections(self, frame):

        # 使用当前流的配置进行处理
        boxes, scores, class_ids = self.detector.detect(frame)
        current_detections = []

        matched_mmsi = set()
        for box, score, class_id in zip(boxes, scores, class_ids):
            # print(box, score, class_id)
            x, y, w, h = box.astype(int)
            x1, y1, x2, y2 = x, y, x + w, y + h
            center = get_bbox_center(x1, y1, x2, y2)

            # 使用当前流的配置检查是否绘制
            if not should_draw_bbox(
                    center,
                    self.config.forbidden_rectangles,
                    self.config.forbidden_polygons
            ):
                continue
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
            else:
                ship_info = "Unknown Ship"
            detection = {
                "box": [x1, y1, x2, y2],
                "label": ship_info,
                "mmsi": nearest_ship["MMSI"] if nearest_ship else None,
                "raw_box": [x1, y1, x2, y2]
            }
            current_detections.append(detection)

        return current_detections
            # if nearest_ship:
            #     ship_info = f"{nearest_ship['ShipName']}, MMSI:{nearest_ship['MMSI']}, 速度:{nearest_ship['Speed']}节"
            #     label= ship_info
            #     matched_mmsi.add(f"{nearest_ship['MMSI']}")
            #     # processed_frame = Plot_one_box(
            #     #     [x1, y1, x2, y2], processed_frame, label,
            #     #     color=(0, 255, 0), line_thickness=2,
            #     #     font_path='utils_demo\\simhei.ttf', font_size=20)
            # else:
            #     label = "Unkown Ship"
        #     processed_frame = Plot_one_box(
        #         [x1, y1, x2, y2], processed_frame, label,
        #         color=(0, 255, 0), line_thickness=2,
        #         font_path='utils_demo\\simhei.ttf', font_size=20)
        #     # print(label)
        # # 调整分辨率并推流
        # resized_frame = cv2.resize(processed_frame, (1280, 960))
        # self.ffmpeg_process.stdin.write(resized_frame.tobytes())

        # 本地显示（可选）
        # cv2.imshow(f'Stream {self.config.stream_id}', resized_frame)
        # cv2.waitKey(1)

    def update_target_cache(self, detections, frame_counter):
        # 更新现有目标
        updated_targets = []

        # 阶段1：尝试匹配现有目标
        for target in self.target_cache:
            best_match = None
            max_iou = 0

            for idx, det in enumerate(detections):
                iou = self.calculate_iou(target.box, det["box"])
                if iou > max_iou and iou > self.iou_threshold:
                    max_iou = iou
                    best_match = idx

            if best_match is not None:
                # 更新匹配到的目标
                det = detections.pop(best_match)
                new_box = self.smooth_position(target.box, det["box"])
                target.box = new_box
                target.label = det["label"]
                target.last_seen = frame_counter
                target.age += 1
                updated_targets.append(target)

        # 阶段2：添加新检测到的目标
        for det in detections:
            new_target = TrackedTarget(
                box=det["box"],
                label=det["label"],
                mmsi=det["mmsi"]
            )
            new_target.last_seen = frame_counter
            updated_targets.append(new_target)

        # 阶段3：清理过期目标
        self.target_cache = [
            t for t in updated_targets
            if (frame_counter - t.last_seen) <= self.cache_max_age
        ]

    def smooth_position(self, old_box, new_box, alpha=0.5):
        """指数加权平滑"""
        return [
            int(old * alpha + new * (1 - alpha))
            for old, new in zip(old_box, new_box)
        ]

    def calculate_iou(self, box1, box2):
        """
        计算两个矩形框之间的交并比（IoU）

        参数:
        box1, box2: 包含四个值的列表或元组 (x1, y1, x2, y2)，分别表示矩形框的左上角和右下角的坐标

        返回:
        float: 交并比值，范围在 0 到 1 之间
        """

        # 计算交集区域的左上角和右下角坐标
        x1_inter = max(box1[0], box2[0])
        y1_inter = max(box1[1], box2[1])
        x2_inter = min(box1[2], box2[2])
        y2_inter = min(box1[3], box2[3])

        # 计算交集的面积
        inter_width = max(0, x2_inter - x1_inter)
        inter_height = max(0, y2_inter - y1_inter)
        intersection_area = inter_width * inter_height

        # 计算两个框的面积
        area1 = (box1[2] - box1[0]) * (box1[3] - box1[1])
        area2 = (box2[2] - box2[0]) * (box2[3] - box2[1])

        # 计算并集的面积
        union_area = area1 + area2 - intersection_area

        # 计算IoU
        iou = intersection_area / union_area if union_area != 0 else 0

        return iou
    def draw_all_targets(self, frame):
        for target in self.target_cache:
            x1, y1, x2, y2 = map(int, target.box)
            alpha = min(target.age / 10.0, 1.0)  # 渐变动画效果

            # 根据目标新鲜度调整颜色
            color = (0, 255, 0)
            frame = Plot_one_box(
                [x1, y1, x2, y2], frame, target.label,
                color=color, line_thickness=2,
                font_path='weights/simhei.ttf', font_size=20
            )
        return frame

