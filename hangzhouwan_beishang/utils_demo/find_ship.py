import numpy as np
import joblib
import math
import pandas as pd
# import config
# 地球半径（千米）
EARTH_RADIUS = 6371.0

def haversine(lon1, lat1, lon2, lat2):
    """计算两个经纬度点之间的距离（以千米为单位）"""
    lon1, lat1, lon2, lat2 = map(math.radians, [lon1, lat1, lon2, lat2])
    dlon, dlat = lon2 - lon1, lat2 - lat1
    a = math.sin(dlat / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2
    c = 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))
    return EARTH_RADIUS * c

def beixia_predict_longitude_latitude(x1, y1, x2, y2, model_path=r'D:\huangchao\hangzhouwan_beishang\weights\0121_random_forest_model.pkl'):
    """
    通过检测框坐标拟合出经纬度
    :param x1: 检测框左上角 x 坐标
    :param y1: 检测框左上角 y 坐标
    :param x2: 检测框右下角 x 坐标
    :param y2: 检测框右下角 y 坐标
    :param model_path: 模型路径
    :return: 拟合的经纬度 (lon1, lat1)
    """
    # 加载模型
    forest_model = joblib.load(model_path)

    # 预测经纬度
    input_data = np.array([[x1, y1, x2, y2]])
    input_df = pd.DataFrame(input_data, columns=['x1', 'y1', 'x2', 'y2'])
    # 使用 DataFrame 进行预测
    prediction = forest_model.predict(input_df)
    lon1, lat1 = prediction[0][0], prediction[0][1]
    return lon1, lat1

def beishang_predict_longitude_latitude(x1, y1, x2, y2, model_path=r'D:\huangchao\hangzhouwan_beishang\weights\beishang_x-l.pkl'):
    """
    通过检测框坐标拟合出经纬度
    :param x1: 检测框左上角 x 坐标
    :param y1: 检测框左上角 y 坐标
    :param x2: 检测框右下角 x 坐标
    :param y2: 检测框右下角 y 坐标
    :param model_path: 模型路径
    :return: 拟合的经纬度 (lon1, lat1)
    """
    # 加载模型
    forest_model = joblib.load(model_path)
    x = (x1 + x2) / 2
    y = (y1 + y2) / 2
    # 预测经纬度
    input_data = np.array([[x, y]])
    input_df = pd.DataFrame(input_data, columns=['x', 'y'])
    # 使用 DataFrame 进行预测
    prediction = forest_model.predict(input_df)
    lon1, lat1 = prediction[0][0], prediction[0][1]
    return lon1, lat1

def nanshang_predict_longitude_latitude(x1, y1, x2, y2, model_path=r'D:\huangchao\hangzhouwan_beishang\weights\beishang_x-l.pkl'):
    """
    通过检测框坐标拟合出经纬度
    :param x1: 检测框左上角 x 坐标
    :param y1: 检测框左上角 y 坐标
    :param x2: 检测框右下角 x 坐标
    :param y2: 检测框右下角 y 坐标
    :param model_path: 模型路径
    :return: 拟合的经纬度 (lon1, lat1)
    """
    # 加载模型
    forest_model = joblib.load(model_path)
    x = (x1 + x2) / 2
    y = (y1 + y2) / 2
    # 预测经纬度
    input_data = np.array([[x, y]])
    input_df = pd.DataFrame(input_data, columns=['x', 'y'])
    # 使用 DataFrame 进行预测
    prediction = forest_model.predict(input_df)
    lon1, lat1 = prediction[0][0], prediction[0][1]
    return lon1, lat1

def nanxia_predict_longitude_latitude(x1, y1, x2, y2, model_path=r'D:\huangchao\hangzhouwan_beishang\weights\beishang_x-l.pkl'):
    """
    通过检测框坐标拟合出经纬度
    :param x1: 检测框左上角 x 坐标
    :param y1: 检测框左上角 y 坐标
    :param x2: 检测框右下角 x 坐标
    :param y2: 检测框右下角 y 坐标
    :param model_path: 模型路径
    :return: 拟合的经纬度 (lon1, lat1)
    """
    # 加载模型
    forest_model = joblib.load(model_path)
    x = (x1 + x2) / 2
    y = (y1 + y2) / 2
    # 预测经纬度
    input_data = np.array([[x, y]])
    input_df = pd.DataFrame(input_data, columns=['x', 'y'])
    # 使用 DataFrame 进行预测
    prediction = forest_model.predict(input_df)
    lon1, lat1 = prediction[0][0], prediction[0][1]
    return lon1, lat1
def find_nearest_ship(lon1, lat1, ship_data_dict,matched_mmsi, max_distance=0.5):
    """
    从 AIS 数据字典中筛选出距离最短且满足 max_distance 内的船舶
    :param lon1: 目标经度
    :param lat1: 目标纬度
    :param ship_data_dict: AIS 数据字典
    :param max_distance: 最大距离（千米）
    :return: 距离最短且满足条件的船舶 AIS 数据，如果未找到则返回 None
    """
    nearest_ship = None
    min_distance = float('inf')

    for mmsi, ship_data in ship_data_dict.items():
        if mmsi in matched_mmsi:
            continue

        lon2, lat2 = ship_data['Longitude'], ship_data['Latitude']
        distance = haversine(lon1, lat1, lon2, lat2)

        if distance < min_distance and distance <= max_distance:
            min_distance = distance
            nearest_ship = ship_data

    return nearest_ship

# 加载模型
def predict_center_coordinates(longitude, latitude,model_path='D:\Project\hangzhouwan\weights\center_random_forest_model.pkl'):
    """
    基于随机森林模型预测检测框中心坐标。

    参数:
        longitude (float): 经度
        latitude (float): 纬度

    返回:
        x (float): 检测框中心 x 坐标
        y (float): 检测框中心 y 坐标
    """
    # 将输入转换为模型所需的格式
    input_data = pd.DataFrame([[longitude, latitude]], columns=['Longitude', 'Latitude'])

    # 调用模型进行预测
    center_model = joblib.load(model_path)
    prediction = center_model.predict(input_data)

    # 提取预测结果
    x, y = prediction[0][0], prediction[0][1]

    return x, y
# 示例调用
if __name__ == '__main__':
    # 示例检测框坐标
    new_x1, new_y1, new_x2, new_y2 = 975, 639, 1017, 663

    # 拟合经纬度
    lon1, lat1 = predict_longitude_latitude(new_x1, new_y1, new_x2, new_y2)
    print(f"拟合的经纬度: Longitude={lon1}, Latitude={lat1}")

    # 示例 AIS 数据字典
    ship_data_dict = {
        413443740: {'MMSI': 413443740, 'Longitude': 121.093027, 'Latitude': 30.58551, 'Speed': 0.1, 'Distance': 4.196036446492805, 'Timestamp': 1736404549},
        413470630: {'MMSI': 413470630, 'Longitude': 121.093133, 'Latitude': 30.585427, 'Speed': 0.2, 'Distance': 4.20079003510769, 'Timestamp': 1736404543},
        413791694: {'MMSI': 413791694, 'Longitude': 121.072322, 'Latitude': 30.596653, 'Speed': 0.6, 'Distance': 3.623031545459741, 'Timestamp': 1736404534},
        413841942: {'MMSI': 413841942, 'Longitude': 121.076062, 'Latitude': 30.595695, 'Speed': 0.1, 'Distance': 3.7193599689766303, 'Timestamp': 1736404543}
    }

    # 查找最近的船舶
    nearest_ship = find_nearest_ship(lon1, lat1, ship_data_dict)
    if nearest_ship:
        print(f"最近的船舶 AIS 数据: {nearest_ship}")
    else:
        print("未找到满足条件的船舶")