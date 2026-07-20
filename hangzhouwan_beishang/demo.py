import cv2
import numpy as np
import onnxruntime
import argparse
import logging
import time
import threading
import subprocess
import sys
import os
from utils_demo import getais
from utils_demo.plot import Plot_one_box
from utils_demo.method import get_bbox_center, should_draw_bbox
from utils_demo.find_ship import haversine,beishang_predict_longitude_latitude,beixia_predict_longitude_latitude, find_nearest_ship



# 区域设置和全局变量
logging.basicConfig(level=logging.INFO, format="%(asctime)s - %(levelname)s - %(message)s")
###检测类
class YOLOv7:
    def __init__(self, path, conf_thres=0.2, iou_thres=0.2):
        self.conf_threshold = conf_thres
        self.iou_threshold = iou_thres
        self.class_names = ['ship']
        # Initialize model
        self.session = onnxruntime.InferenceSession(path, providers=['CUDAExecutionProvider'])
        model_inputs = self.session.get_inputs()
        self.input_names = [model_inputs[i].name for i in range(len(model_inputs))]
        self.input_shape = model_inputs[0].shape
        self.input_height = 640
        self.input_width = 640

        model_outputs = self.session.get_outputs()
        self.output_names = [model_outputs[i].name for i in range(len(model_outputs))]
        self.has_postprocess = 'score' in self.output_names

    def detect(self, image):
        input_tensor = self.prepare_input(image)

        # Perform inference on the image
        outputs = self.session.run(self.output_names, {self.input_names[0]: input_tensor})

        if self.has_postprocess:
            boxes, scores, class_ids = self.parse_processed_output(outputs)

        else:
            # Process output data
            boxes, scores, class_ids = self.process_output(outputs)

        return boxes, scores, class_ids

    def prepare_input(self, image):
        self.img_height, self.img_width = image.shape[:2]

        input_img = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)

        # Resize input image
        input_img = cv2.resize(input_img, (self.input_width, self.input_height))

        # Scale input pixel values to 0 to 1
        input_img = input_img / 255.0
        input_img = input_img.transpose(2, 0, 1)
        input_tensor = input_img[np.newaxis, :, :, :].astype(np.float32)
        return input_tensor

    def process_output(self, output):
        predictions = np.squeeze(output[0])

        # Filter out object confidence scores below threshold
        obj_conf = predictions[:, 4]
        predictions = predictions[obj_conf > self.conf_threshold]
        obj_conf = obj_conf[obj_conf > self.conf_threshold]

        # Multiply class confidence with bounding box confidence
        predictions[:, 5:] *= obj_conf[:, np.newaxis]

        # Get the scores
        scores = np.max(predictions[:, 5:], axis=1)

        # Filter out the objects with a low score
        valid_scores = scores > self.conf_threshold
        predictions = predictions[valid_scores]
        scores = scores[valid_scores]

        # Get the class with the highest confidence
        class_ids = np.argmax(predictions[:, 5:], axis=1)

        # Get bounding boxes for each object
        boxes = self.extract_boxes(predictions)

        # Apply non-maxima suppression to suppress weak, overlapping bounding boxes
        # indices = nms(boxes, scores, self.iou_threshold)
        # 使用 cv2.dnn.NMSBoxes 进行非极大值抑制
        indices = cv2.dnn.NMSBoxes(boxes.tolist(), scores.tolist(), self.conf_threshold, self.iou_threshold)

        # 提取索引数组
        if len(indices) > 0:
            indices = indices.flatten()  # 将索引数组展平
        else:
            indices = []  # 如果没有检测到任何框，设置为空列表

        # 根据 indices 过滤 boxes, scores, class_ids
        filtered_boxes = [boxes[i] for i in indices]
        filtered_scores = [scores[i] for i in indices]
        filtered_class_ids = [class_ids[i] for i in indices]

        return filtered_boxes, filtered_scores, filtered_class_ids
        # indices = cv2.dnn.NMSBoxes(boxes.tolist(), scores.tolist(), self.conf_threshold, self.iou_threshold).flatten()
        #
        # return boxes[indices], scores[indices], class_ids[indices]

    def parse_processed_output(self, outputs):

        scores = np.squeeze(outputs[self.output_names.index('score')])
        predictions = outputs[self.output_names.index('batchno_classid_x1y1x2y2')]

        # Filter out object scores below threshold
        valid_scores = scores > self.conf_threshold
        predictions = predictions[valid_scores, :]
        scores = scores[valid_scores]

        # Extract the boxes and class ids
        # TODO: Separate based on batch number
        batch_number = predictions[:, 0]
        class_ids = predictions[:, 1]
        boxes = predictions[:, 2:]

        # In postprocess, the x,y are the y,x
        boxes = boxes[:, [1, 0, 3, 2]]

        # Rescale boxes to original image dimensions
        boxes = self.rescale_boxes(boxes)

        return boxes, scores, class_ids

    def extract_boxes(self, predictions):
        # Extract boxes from predictions
        boxes = predictions[:, :4]

        # Scale boxes to original image dimensions
        boxes = self.rescale_boxes(boxes)

        # Convert boxes to xywh format
        boxes_ = np.copy(boxes)
        boxes_[..., 0] = boxes[..., 0] - boxes[..., 2] * 0.5
        boxes_[..., 1] = boxes[..., 1] - boxes[..., 3] * 0.5
        return boxes_

    def rescale_boxes(self, boxes):

        # Rescale boxes to original image dimensions
        input_shape = np.array([self.input_width, self.input_height, self.input_width, self.input_height])
        boxes = np.divide(boxes, input_shape, dtype=np.float32)
        boxes *= np.array([self.img_width, self.img_height, self.img_width, self.img_height])
        return boxes

    def draw_detections(self, image, boxes, scores, class_ids):
        """
        使用 Plot_one_box 绘制检测框和标签。
        :param image: 输入图像（numpy 数组）
        :param boxes: 检测框列表，格式为 (x, y, w, h)
        :param scores: 置信度列表
        :param class_ids: 类别 ID 列表
        :return: 绘制后的图像
        """
        for box, score, class_id in zip(boxes, scores, class_ids):
            # 将 (x, y, w, h) 转换为 (x1, y1, x2, y2)
            x, y, w, h = box.astype(int)
            xyxy = [x, y, x + w, y + h]

            # 构建标签
            label = f'{self.class_names[class_id]}'

            # 调用 Plot_one_box 绘制检测框和标签
            image = Plot_one_box(
                xyxy, image, label=label, color=(0, 255, 0),  # 绿色框
                line_thickness=2, font_path='.\\weights\\simhei.ttf',
                font_size=20, line_spacing=10
            )
        return image

# 视频流处理类（每个实例处理一个视频流）
class VideoStreamHandler:
    def __init__(self, stream_url, rtmp_url, detector, config):
        """
        :param stream_url: 视频流地址
        :param rtmp_url: RTMP推流地址
        :param detector: YOLOv7检测器实例
        :param config: 流配置对象（StreamConfig）
        """
        self.stream_url = stream_url
        self.rtmp_url = rtmp_url
        self.detector = detector
        self.config = config  # 注入流配置
        self.frames = []  # 存储视频帧
        self.frame_lock = threading.Lock()  # 线程锁
        self.running = False  # 运行状态
        self.ffmpeg_process = None  # FFmpeg进程


    def start(self):
        """启动视频流处理线程"""
        self.running = True
        # 启动视频流读取线程
        self.read_thread = threading.Thread(target=self.read_video_stream, daemon=True)
        self.read_thread.start()
        # 启动检测和推流线程
        self.detect_thread = threading.Thread(target=self.detect_and_stream, daemon=True)
        self.detect_thread.start()

    def stop(self):
        """停止处理并释放资源"""
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
            self.rtmp_url
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
        while self.running:
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
                        processed_frame = Plot_one_box(
                            [x1, y1, x2, y2], processed_frame, label,
                            color=(0, 255, 0), line_thickness=2,
                            font_path='weights/simhei.ttf', font_size=20)
                    else:
                        label = "Unkown Ship"
                        processed_frame = Plot_one_box(
                            [x1, y1, x2, y2], processed_frame, label,
                            color=(0, 255, 0), line_thickness=2,
                            font_path='weights/simhei.ttf', font_size=20)
                    # print(label)
            # 调整分辨率并推流
            resized_frame = cv2.resize(processed_frame, (1280, 960))
            self.ffmpeg_process.stdin.write(resized_frame.tobytes())

            # 本地显示（可选）
            # cv2.imshow(f'Stream {self.config.stream_id}', resized_frame)
            # cv2.waitKey(1)

class StreamConfig:
    def __init__(self,
                 stream_url: str,
                 stream_id: str,
                 forbidden_rectangles: list,
                 forbidden_polygons: list,
                 predict_coord_func: callable,
                 **kwargs):
        """
        :param stream_id: 流标识（如'A'/'B'）
        :param forbidden_rectangles: 该流的禁止矩形区域
        :param forbidden_polygons: 该流的禁止多边形区域zuiji
        :param predict_coord_func: 该流专用的坐标转经纬度函数
        """
        self.stream_url = stream_url
        self.stream_id = stream_id
        self.forbidden_rectangles = forbidden_rectangles
        self.forbidden_polygons = forbidden_polygons
        self.predict_coord_func = predict_coord_func
        self.extra_params = kwargs  # 扩展参数（如相机参数）

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(levelname)s - %(message)s",
    handlers=[logging.StreamHandler()]
)

#### 区域设置 #####
#### A ####
A_forbidden_rectangles = [
    ((1480, 0), (2560, 630)),  # 矩形1
    ((247, 855), (275, 888)),  # 矩形1
    ((2295, 895), (2315, 927))  # 矩形1
]
# 禁止标注的多边形区域
A_forbidden_polygons = [
    [(0, 0), (0, 640), (710, 620), (1260, 620), (1260, 0)]  # 多边形1
]
#### B ####
B_forbidden_rectangles = [
    ((0, 0), (2560, 210)),  # 矩形1
]
# 禁止标注的多边形区域
B_forbidden_polygons = [
    [(2160, 210), (2560, 280), (2560, 210)]  # 多边形1
]
#### C ####
C_forbidden_rectangles = [
    ((1480, 0), (2560, 630)),  # 矩形1
    ((247, 855), (275, 888)),  # 矩形1
    ((2295, 895), (2315, 927))  # 矩形1
]
# 禁止标注的多边形区域
C_forbidden_polygons = [
    [(0, 0), (0, 640), (710, 620), (1260, 620), (1260, 0)]  # 多边形1
]
#### D ####
D_forbidden_rectangles = [
    ((0, 0), (2560, 210)),  # 矩形1
]
# 禁止标注的多边形区域
D_forbidden_polygons = [
    [(2160, 210), (2560, 280), (2560, 210)]  # 多边形1
]
#################

# 全局变量
video_frames = []  # 存储视频帧
detection_results = []  # 存储检测结果
video_lock = threading.Lock()  # 用于线程安全
detection_lock = threading.Lock()  # 用于线程安全
yolov7_detector = YOLOv7(r'./weights/best.onnx', conf_thres=0.1, iou_thres=0.1)

# --------------------------
# 北下流A的配置
# --------------------------
A_config = StreamConfig(
    stream_url = os.environ.get('STREAM_A_INPUT_URL', ''),
    stream_id="A",
    forbidden_rectangles=A_forbidden_rectangles,
    forbidden_polygons=A_forbidden_polygons,
    predict_coord_func=beixia_predict_longitude_latitude,
    camera_param=0.5  # 可扩展其他参数
)

# --------------------------
# 北上流B的配置
# --------------------------

B_config = StreamConfig(
    stream_url = os.environ.get('STREAM_B_INPUT_URL', ''),
    stream_id="B",
    forbidden_rectangles=B_forbidden_rectangles,
    forbidden_polygons=B_forbidden_polygons,
    predict_coord_func=beishang_predict_longitude_latitude,
    camera_param=0.8
)

# # --------------------------
# # 南下流C的配置
# # --------------------------
# C_config = StreamConfig(
#     stream_url = '<EZVIZ地址已脱敏，改用环境变量>',
#     stream_id="C",
#     forbidden_rectangles=C_forbidden_rectangles,
#     forbidden_polygons=C_forbidden_polygons,
#     predict_coord_func=beixia_predict_longitude_latitude,
#     camera_param=0.5  # 可扩展其他参数
# )
#
# # --------------------------
# # 南上流D的配置
# # --------------------------
#
# D_config = StreamConfig(
#     stream_url = '<EZVIZ地址已脱敏，改用环境变量>',
#     stream_id="D",
#     forbidden_rectangles=D_forbidden_rectangles,
#     forbidden_polygons=D_forbidden_polygons,
#     predict_coord_func=beishang_predict_longitude_latitude,
#     camera_param=0.8
# )

def main():
    detector = YOLOv7(r'./weights/best.onnx', conf_thres=0.1, iou_thres=0.45)

    # 预定义运行时长（24小时）
    RUN_HOURS = 5
    RUN_DURATION = RUN_HOURS * 3600  # 转换为秒
    start_time = time.time()
    # 定义流配置
    streams = [
        {
            'stream_url': os.environ.get('STREAM_A_INPUT_URL', ''),
            'rtmp_url': os.environ.get('STREAM_A_OUTPUT_URL', ''),
            'config': A_config
        },
        {
            'stream_url': os.environ.get('STREAM_B_INPUT_URL', ''),
            'rtmp_url': os.environ.get('STREAM_B_OUTPUT_URL', ''),
            'config': B_config
        }
    ]

    # 创建处理器
    handlers = []
    for stream_info in streams:
        handler = VideoStreamHandler(
            stream_url=stream_info['stream_url'],
            rtmp_url=stream_info['rtmp_url'],
            detector=detector,
            config=stream_info['config']  # 注入配置
        )
        handler.start()
        handlers.append(handler)

    # 启动AIS数据更新线程
    ais_thread = threading.Thread(target=getais.main, daemon=True)
    ais_thread.start()

    # 主循环保持运行
    try:
        # 主循环添加时间检查
        while True:
            # if time.time() - start_time > RUN_DURATION:
            #     logging.info(f"运行{RUN_HOURS}小时，主动重启...")
            #     break  # 跳出循环触发清理
            time.sleep(60)  # 每分钟检查一次

    except KeyboardInterrupt:
        logging.info("用户中断...")
    finally:
        # 清理资源
        for handler in handlers:
            handler.stop()
        # 非零退出码触发nssm重启
        sys.exit(1)  # 关键点：返回非零状态码


if __name__ == '__main__':
    main()