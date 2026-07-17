import cv2
import threading
import subprocess
import time
import logging
import queue
from collections import deque
import utils_demo.config
from utils_demo.method import get_bbox_center, should_draw_bbox
from utils_demo import getais
from utils_demo.plot import Plot_one_box
from utils_demo.find_ship import find_nearest_ship

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

class OptimizedVideoStreamHandler:
    def __init__(self, detector, config):
        self.detector = detector
        self.config = config
        
        # 使用队列替代简单列表，提高线程安全性和性能
        self.frame_queue = queue.Queue(maxsize=3)  # 限制队列大小，避免积压
        self.processed_queue = queue.Queue(maxsize=3)
        
        self.running = False
        self.ffmpeg_process = None
        
        # 性能监控
        self.fps_counter = 0
        self.last_fps_time = time.time()
        self.processing_times = deque(maxlen=30)  # 记录最近30帧的处理时间
        
        # 自适应质量控制
        self.current_quality = "high"  # high, medium, low
        self.quality_adjust_counter = 0

    def start(self):
        self.running = True
        
        # 分离读取、处理、推流为三个独立线程
        self.read_thread = threading.Thread(target=self.read_video_stream, daemon=True)
        self.process_thread = threading.Thread(target=self.process_frames, daemon=True)
        self.stream_thread = threading.Thread(target=self.stream_frames, daemon=True)
        
        self.read_thread.start()
        self.process_thread.start()
        self.stream_thread.start()

    def stop(self):
        self.running = False
        if self.ffmpeg_process:
            try:
                self.ffmpeg_process.stdin.close()
                self.ffmpeg_process.wait(timeout=5)
            except:
                self.ffmpeg_process.kill()

    def get_optimized_ffmpeg_command(self):
        """获取优化后的FFmpeg命令"""
        ffmpeg_path = utils_demo.config.ffmpeg_path
        
        # 根据当前质量等级调整参数
        if self.current_quality == "high":
            preset = "veryfast"
            crf = "25"
            resolution = "1280x960"
            fps = "15"
        elif self.current_quality == "medium":
            preset = "ultrafast"
            crf = "28"
            resolution = "960x720"
            fps = "12"
        else:  # low
            preset = "ultrafast"
            crf = "32"
            resolution = "640x480"
            fps = "10"
        
        command = [
            ffmpeg_path,
            '-y',
            '-f', 'rawvideo',
            '-vcodec', 'rawvideo',
            '-pix_fmt', 'bgr24',
            '-s', resolution,
            '-r', fps,
            '-i', '-',
            
            # 优化的编码参数
            '-c:v', 'libx264',
            '-preset', preset,
            '-tune', 'zerolatency',
            '-profile:v', 'baseline',  # 兼容性更好
            '-level', '3.0',
            '-crf', crf,
            '-g', '15',  # GOP大小，影响延迟
            '-keyint_min', '15',
            '-sc_threshold', '0',  # 禁用场景切换检测
            
            # 缓冲区和延迟优化
            '-bufsize', '1000k',
            '-maxrate', '2000k',
            '-threads', '2',  # 限制线程数
            '-flags', '+cgop',  # 封闭GOP
            '-avioflags', 'direct',  # 直接IO
            '-fflags', '+flush_packets',  # 立即刷新包
            
            # 输出格式
            '-f', 'flv',
            self.config.rtmp_url
        ]
        return command

    def start_ffmpeg_stream(self):
        """启动FFmpeg推流进程"""
        command = self.get_optimized_ffmpeg_command()
        
        # 添加调试信息
        logging.info(f"启动FFmpeg命令: {' '.join(command[:10])}...")
        
        try:
            process = subprocess.Popen(
                command, 
                stdin=subprocess.PIPE, 
                stderr=subprocess.PIPE,
                stdout=subprocess.PIPE,
                bufsize=0,  # 无缓冲
                creationflags=subprocess.CREATE_NO_WINDOW if hasattr(subprocess, 'CREATE_NO_WINDOW') else 0
            )
            
            # 检查进程是否成功启动
            time.sleep(0.5)
            if process.poll() is not None:
                stderr_output = process.stderr.read().decode('utf-8', errors='ignore')
                logging.error(f"FFmpeg启动失败，错误信息: {stderr_output}")
                return None
                
            return process
            
        except Exception as e:
            logging.error(f"启动FFmpeg进程异常: {e}")
            return None

    def read_video_stream(self):
        """优化的视频流读取"""
        reconnect_interval = 3
        
        while self.running:
            cap = None
            try:
                cap = cv2.VideoCapture(self.config.stream_url)
                
                # 优化VideoCapture设置
                cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)  # 减少缓冲
                cap.set(cv2.CAP_PROP_FPS, 15)
                
                if not cap.isOpened():
                    raise ConnectionError(f"无法连接视频流 {self.config.stream_id}")

                logging.info(f"视频流 {self.config.stream_id} 连接成功")
                consecutive_failures = 0

                while self.running:
                    ret, frame = cap.read()
                    if not ret:
                        consecutive_failures += 1
                        if consecutive_failures >= 3:
                            break
                        time.sleep(0.1)
                        continue

                    consecutive_failures = 0
                    
                    # 非阻塞放入队列
                    try:
                        self.frame_queue.put_nowait(frame)
                    except queue.Full:
                        # 队列满时丢弃旧帧
                        try:
                            self.frame_queue.get_nowait()
                            self.frame_queue.put_nowait(frame)
                        except queue.Empty:
                            pass

            except Exception as e:
                logging.error(f"视频流 {self.config.stream_id} 异常: {str(e)}")
            finally:
                if cap:
                    cap.release()
                time.sleep(reconnect_interval)

    def process_frames(self):
        """优化的帧处理"""
        while self.running:
            try:
                # 带超时的获取帧
                frame = self.frame_queue.get(timeout=1)
                
                start_time = time.time()
                
                # 执行检测和处理
                processed_frame = self.detect_and_annotate(frame)
                
                # 记录处理时间
                process_time = time.time() - start_time
                self.processing_times.append(process_time)
                
                # 自适应质量调整
                self.adaptive_quality_control(process_time)
                
                # 放入输出队列
                try:
                    self.processed_queue.put_nowait(processed_frame)
                except queue.Full:
                    try:
                        self.processed_queue.get_nowait()
                        self.processed_queue.put_nowait(processed_frame)
                    except queue.Empty:
                        pass
                        
            except queue.Empty:
                continue
            except Exception as e:
                logging.error(f"帧处理错误: {e}")

    def detect_and_annotate(self, frame):
        """检测和标注函数"""
        boxes, scores, class_ids = self.detector.detect(frame)
        processed_frame = frame.copy()
        
        matched_mmsi = set()
        for box, score, class_id in zip(boxes, scores, class_ids):
            x, y, w, h = box.astype(int)
            x1, y1, x2, y2 = x, y, x + w, y + h
            center = get_bbox_center(x1, y1, x2, y2)

            if should_draw_bbox(center, self.config.forbidden_rectangles, self.config.forbidden_polygons):
                lon1, lat1 = self.config.predict_coord_func(x1, y1, x2, y2)
                nearest_ship = find_nearest_ship(lon1, lat1, getais.ship_data_dict, matched_mmsi)

                if nearest_ship:
                    ship_info = f"{nearest_ship['ShipName']}, MMSI:{nearest_ship['MMSI']}, 速度:{nearest_ship['Speed']}节"
                    matched_mmsi.add(f"{nearest_ship['MMSI']}")
                else:
                    ship_info = "Unknown Ship"

                processed_frame = Plot_one_box(
                    [x1, y1, x2, y2], processed_frame, ship_info,
                    color=(0, 255, 0), line_thickness=2,
                    font_path=utils_demo.config.font_path, font_size=20
                )
        
        return processed_frame

    def stream_frames(self):
        """优化的推流处理"""
        retry_count = 0
        max_retries = 5
        
        while self.running:
            # 确保FFmpeg进程运行
            if not self.ensure_ffmpeg_alive():
                time.sleep(2)
                retry_count += 1
                if retry_count > max_retries:
                    logging.error("FFmpeg重启次数过多，暂停推流")
                    time.sleep(10)
                    retry_count = 0
                continue
            
            try:
                # 获取处理好的帧
                processed_frame = self.processed_queue.get(timeout=1)
                
                # 根据质量等级调整分辨率
                if self.current_quality == "high":
                    target_size = (1280, 960)
                elif self.current_quality == "medium":
                    target_size = (960, 720)
                else:
                    target_size = (640, 480)
                
                resized_frame = cv2.resize(processed_frame, target_size)
                
                # 检查FFmpeg进程是否有效
                if not self.ffmpeg_process or self.ffmpeg_process.poll() is not None:
                    logging.warning(f"FFmpeg进程无效，跳过此帧")
                    continue
                
                # 写入FFmpeg
                try:
                    self.ffmpeg_process.stdin.write(resized_frame.tobytes())
                    self.ffmpeg_process.stdin.flush()  # 强制刷新缓冲区
                    retry_count = 0  # 重置重试计数
                    
                    # FPS统计
                    self.update_fps_counter()
                    
                except (BrokenPipeError, IOError) as pipe_error:
                    logging.warning(f"流 {self.config.stream_id} 推流管道中断: {pipe_error}")
                    self.safe_terminate_ffmpeg()
                    retry_count += 1
                    
            except queue.Empty:
                continue
            except Exception as e:
                logging.error(f"流 {self.config.stream_id} 推流错误: {e}")
                self.safe_terminate_ffmpeg()
                retry_count += 1

    def adaptive_quality_control(self, process_time):
        """自适应质量控制"""
        self.quality_adjust_counter += 1
        
        # 每30帧检查一次
        if self.quality_adjust_counter >= 30:
            avg_process_time = sum(self.processing_times) / len(self.processing_times)
            
            # 根据处理时间调整质量
            if avg_process_time > 0.1 and self.current_quality != "low":
                self.current_quality = "medium" if self.current_quality == "high" else "low"
                logging.info(f"降低质量到: {self.current_quality}")
                self.restart_ffmpeg()
            elif avg_process_time < 0.05 and self.current_quality != "high":
                self.current_quality = "medium" if self.current_quality == "low" else "high"
                logging.info(f"提升质量到: {self.current_quality}")
                self.restart_ffmpeg()
            
            self.quality_adjust_counter = 0

    def restart_ffmpeg(self):
        """重启FFmpeg进程以应用新参数"""
        self.safe_terminate_ffmpeg()
        time.sleep(1)

    def ensure_ffmpeg_alive(self):
        """确保FFmpeg进程正常运行"""
        if self.ffmpeg_process and self.ffmpeg_process.poll() is None:
            return True

        logging.info(f"正在重新启动流 {self.config.stream_id} 的FFmpeg进程...")
        self.safe_terminate_ffmpeg()  # 清理旧进程

        try:
            self.ffmpeg_process = self.start_ffmpeg_stream()
            if self.ffmpeg_process is None:
                return False
                
            time.sleep(1)
            if self.ffmpeg_process.poll() is not None:
                logging.error(f"FFmpeg进程启动后立即退出")
                return False
                
            logging.info(f"流 {self.config.stream_id} FFmpeg进程启动成功")
            return True
            
        except Exception as e:
            logging.error(f"FFmpeg启动失败: {e}")
            self.safe_terminate_ffmpeg()
            return False

    def safe_terminate_ffmpeg(self):
        """安全终止FFmpeg进程"""
        if self.ffmpeg_process:
            try:
                self.ffmpeg_process.stdin.close()
                self.ffmpeg_process.terminate()
                self.ffmpeg_process.wait(timeout=3)
            except:
                try:
                    self.ffmpeg_process.kill()
                except:
                    pass
            self.ffmpeg_process = None

    def update_fps_counter(self):
        """更新FPS计数器"""
        self.fps_counter += 1
        current_time = time.time()
        
        if current_time - self.last_fps_time >= 5:  # 每5秒输出一次FPS
            fps = self.fps_counter / (current_time - self.last_fps_time)
            logging.info(f"流 {self.config.stream_id} FPS: {fps:.2f}, 质量: {self.current_quality}")
            self.fps_counter = 0
            self.last_fps_time = current_time
