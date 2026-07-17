import cv2
import numpy as np
import onnxruntime
from utils_demo.plot import Plot_one_box
import utils_demo.config

class YOLOv7:
    def __init__(self, model_path, conf_thres=0.1, iou_thres=0.1):
        self.conf_threshold = conf_thres
        self.iou_threshold = iou_thres
        self.class_names = ['ship']

        # 初始化ONNX模型
        self.session = onnxruntime.InferenceSession(
            model_path,
            providers=['CUDAExecutionProvider']
        )
        self._initialize_model_parameters()

    def _initialize_model_parameters(self):
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
                line_thickness=2, font_path= utils_demo.config.font_path,
                font_size=20, line_spacing=10
            )
        return image