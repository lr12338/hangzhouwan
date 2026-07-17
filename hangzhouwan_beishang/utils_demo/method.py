import cv2
import numpy as np


# 频道：test1
# token：007eJxTYFjf4ZW/8uvaS4v+Wge7rl7h+G4dn8tTK2Wx1P8suinnapkUGMyNjE2NjNJMTM1Sk0zSDCwSUywsjAwsk1MtklNSEo3Nri7pSG8IZGSw1bvBzMgAgSA+K0NJanGJIQMDAIxZH/Q=
# APPID：723522f456eb4f08ad88209ce8cdda36
# APP证书：028be94dd4664d198ef291efc48ad627

####区域设置
# 禁止标注的矩形区域
forbidden_rectangles = [
    ((1480, 0), (2560, 630)),  # 矩形1
    ((247, 855), (275, 888)),  # 矩形1
    ((2295, 895), (2315, 927))  # 矩形1
]

# 禁止标注的多边形区域
forbidden_polygons = [
    [(0, 0), (0, 640), (710, 620), (1260, 620), (1260, 0)]  # 多边形1
]
def get_bbox_center(x1, y1, x2, y2):
    """计算标注框的中心点"""
    center_x = (x1 + x2) / 2
    center_y = (y1 + y2) / 2
    return (center_x, center_y)
def is_point_in_rectangle(point, rectangle):
    """
    判断点是否在矩形区域内
    :param point: 点的坐标 (x, y)
    :param rectangle: 矩形的左上角和右下角坐标 ((x1, y1), (x2, y2))
    :return: True 如果在矩形内，否则 False
    """
    (x, y) = point
    (rect_x1, rect_y1), (rect_x2, rect_y2) = rectangle
    return rect_x1 <= x <= rect_x2 and rect_y1 <= y <= rect_y2
def is_point_in_polygon(point, polygon):
    """
    判断点是否在多边形区域内
    :param point: 点的坐标 (x, y)
    :param polygon: 多边形的顶点列表 [(x1, y1), (x2, y2), ...]
    :return: True 如果在多边形内，否则 False
    """
    x, y = point
    n = len(polygon)
    inside = False

    p1x, p1y = polygon[0]
    for i in range(1, n + 1):
        p2x, p2y = polygon[i % n]
        if y > min(p1y, p2y):
            if y <= max(p1y, p2y):
                if x <= max(p1x, p2x):
                    if p1y != p2y:
                        xinters = (y - p1y) * (p2x - p1x) / (p2y - p1y) + p1x
                    if p1x == p2x or x <= xinters:
                        inside = not inside
        p1x, p1y = p2x, p2y

    return inside
def should_draw_bbox(center, forbidden_rectangles, forbidden_polygons):
    """
    判断是否应该绘制标注框
    :param center: 标注框中心点 (x, y)
    :param forbidden_rectangles: 禁止标注的矩形区域列表
    :param forbidden_polygons: 禁止标注的多边形区域列表
    :return: True 如果可以绘制，否则 False
    """
    # 检查矩形区域
    for rect in forbidden_rectangles:
        if is_point_in_rectangle(center, rect):
            return False

    # 检查多边形区域
    for polygon in forbidden_polygons:
        if is_point_in_polygon(center, polygon):
            return False

    return True


