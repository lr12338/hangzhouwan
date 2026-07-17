import cv2
import numpy as np
from PIL import ImageFont, ImageDraw, Image
import config


def Plot_one_box(xyxy, im0, label=None, color=None, line_thickness=1, font_path=config.font_path,
                 font_size=10, line_spacing=10):
    """
    在图片上绘制检测框和标签。

    :param xyxy: 检测框坐标，格式为 (x1, y1, x2, y2)
    :param im0: 输入图片（numpy 数组）
    :param label: 标签文本
    :param color: 框和文本颜色（BGR 格式）
    :param line_thickness: 框的线宽
    :param font_path: 字体文件路径
    :param font_size: 字体大小
    :param line_spacing: 行间距（像素）
    :return: 绘制后的图片
    """
    # 解析坐标
    x1, y1, x2, y2 = map(int, xyxy)

    # 绘制检测框
    cv2.rectangle(im0, (x1, y1), (x2, y2), color, line_thickness)

    if label:
        # 使用PIL绘制多行文本
        pil_im = Image.fromarray(im0)
        draw = ImageDraw.Draw(pil_im, 'RGBA')  # 使用 RGBA 模式支持透明度
        font = ImageFont.truetype(font_path, font_size)  # 加载字体

        # 将label按空格拆分，并换行
        lines = label.split(', ')  # 按空格拆分

        # 使用 getbbox 获取文本高度
        bbox = font.getbbox("Test")  # 返回 (left, top, right, bottom)
        text_height = bbox[3] - bbox[1]  # 计算高度 (bottom - top)

        # 确保 color 是有效的元组
        if isinstance(color, (list, np.ndarray)):
            color = tuple(map(int, color))  # 将列表或 numpy 数组转换为元组
        elif not isinstance(color, tuple):
            color = (255, 0, 0)  # 默认红色

        # 计算文本区域的总高度（包括行间距）
        total_text_height = len(lines) * (text_height + line_spacing) - line_spacing

        # 设置标签框与检测框的间距
        spacing = 3  # 间距大小（像素）

        # 在文本底层绘制半透明阴影底色
        shadow_color = (0, 0, 0, 64)  # 半透明黑色 (R, G, B, A)
        text_bg_x1 = x1
        text_bg_y1 = y1 - total_text_height - spacing -5  # 上移文本框，增加间距
        text_bg_x2 = x1 + max(font.getlength(line) for line in lines) + 10  # 根据最长文本行计算宽度
        text_bg_y2 = y1 - spacing +5# 上移文本框，增加间距

        # 绘制半透明阴影底色
        draw.rectangle([text_bg_x1, text_bg_y1, text_bg_x2, text_bg_y2], fill=shadow_color)

        # 逐行绘制文本（增加行间距）
        for i, line in enumerate(lines):
            draw.text(
                (x1 + 5, y1 - total_text_height - spacing + i * (text_height + line_spacing)),
                line, font=font, fill=color
            )

        im0 = np.array(pil_im)

    return im0


def test_plot_one_box(image_path, xyxy, label, color=(0, 255, 0), line_thickness=1, font_path=config.ffmpeg_path, font_size=10, line_spacing=5):
    """
    测试绘制检测框和标签。

    :param image_path: 图片路径
    :param xyxy: 检测框坐标，格式为 (x1, y1, x2, y2)
    :param label: 标签文本
    :param color: 框和文本颜色（BGR 格式）
    :param line_thickness: 框的线宽
    :param font_path: 字体文件路径
    :param font_size: 字体大小
    """
    # 加载图片
    im0 = cv2.imread(image_path)
    if im0 is None:
        print(f"Error: Unable to load image from {image_path}")
        return

    # 绘制检测框和标签
    im0 = Plot_one_box(xyxy, im0, label=label, color=color, line_thickness=line_thickness, font_path=font_path, font_size=font_size, line_spacing=line_spacing)

    # 显示结果
    cv2.imshow("Image with Box and Label", im0)
    cv2.waitKey(0)
    cv2.destroyAllWindows()


# 示例调用
if __name__ == "__main__":
    image_path = "test.jpg"  # 替换为你的图片路径
    xyxy = [500, 500, 520, 520]  # 检测框坐标 (x1, y1, x2, y2)
    label = "船名:华苑7号 MMSI:41329523 Speed:7.3节 航迹向:293.2"  # 标签文本
    color = (0, 255, 0)  # 绿色

    # 测试绘制
    test_plot_one_box(image_path, xyxy, label, color)