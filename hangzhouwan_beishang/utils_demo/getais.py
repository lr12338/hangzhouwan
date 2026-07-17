


#!/usr/bin/env python
# -*- coding: utf-8 -*-
# @Time    : 2025/1/8 14:52
# @Author  : lurui
# @File    : getais.py
# @Software: PyCharm

import threading
import time
import math
import re
import logging
from pyais import decode
from paho.mqtt.client import Client
import requests
import json
# 配置
class Config:
    BROKER_HOST = "iot.hifleet.com"
    BROKER_PORT = 1883
    CLIENT_ID = "hangzhouwan_lurui"
    USERNAME = "hangzhouwan_lurui"
    PASSWORD = "hangzhouwan_lurui"
    # CLIENT_ID = "mqtt_hyw"
    # USERNAME = "mqtt_hyw"
    # PASSWORD = "mqtt_hyw"
    AIS_TOPICS = ["upAIS/base_2250", "upAIS/base_2251"]  # 订阅的 AIS 主题
    B_POINT = (121.035267, 30.559733)
    EARTH_RADIUS = 6371.0  # 地球半径（千米）
    MAX_DISTANCE = 30  # 距离标记点 A 的最大距离（千米）
    MIN_SPEED = 0  # 最小速度（节）
    DATA_TIMEOUT = 30  # 数据超时时间（秒）

# 日志配置
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(levelname)s - %(message)s",
    handlers=[logging.StreamHandler()]
)

# 全局 AIS 数据字典
ship_data_dict = {}
ais_lock = threading.Lock()  # 用于线程安全

def haversine(lon1, lat1, lon2, lat2):
    """计算两个经纬度点之间的距离（以千米为单位）"""
    lon1, lat1, lon2, lat2 = map(math.radians, [lon1, lat1, lon2, lat2])
    dlon, dlat = lon2 - lon1, lat2 - lat1
    a = math.sin(dlat / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2
    c = 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))
    return Config.EARTH_RADIUS * c

def on_connect(client, userdata, flags, rc):
    """MQTT 回调函数 - 连接成功"""
    if rc == 0:
        logging.info(f"Connected to MQTT Broker: {Config.BROKER_HOST}")
        for topic in Config.AIS_TOPICS:
            client.subscribe(topic)
            logging.info(f"Subscribed to topic: {topic}")
    else:
        logging.error(f"Connection failed with result code {rc}")

def on_message(client, userdata, msg):
    """MQTT 回调函数 - 消息处理"""
    try:
        raw_message = msg.payload.decode('utf-8')
        timestamp_match = re.search(r'\*(\d{10})$', raw_message)
        timestamp = int(timestamp_match.group(1)) if timestamp_match else None

        raw_message = raw_message.replace(f'*{timestamp_match.group(1)}', '')
        logging.debug(f"Received AIS Message: {raw_message}, {timestamp}")
        ais = raw_message.encode('ascii')
        message = decode(ais)
        # print(message)

        # 提取 MMSI
        mmsi = message.mmsi
        # print(findname(mmsi))

        # 初始化船舶信息
        ship_info = ship_data_dict.get(mmsi, {})
        ship_info["MMSI"] = mmsi
        ship_info["Timestamp"] = timestamp
        try:
            ship_info["ShipName"] = findname(mmsi)
        except:
            # ship_info["ShipName"] = None
            pass

        # 根据消息类型提取属性
        if message.msg_type in [1, 3, 18,4]:  # MessageType1, MessageType3, MessageType18
            ship_info["Longitude"] = getattr(message, 'lon', None)
            ship_info["Latitude"] = getattr(message, 'lat', None)
            ship_info["Speed"] = getattr(message, 'speed', None)
            ship_info["Course"] = getattr(message, 'course', None)

        # elif message.msg_type == 24:  # MessageType24
        #     ship_info["ShipName"] = getattr(message, 'shipname', None)

        # # 检查数据是否完整
        # if not all([ship_info.get("Longitude"), ship_info.get("Latitude"), ship_info.get("Speed"), timestamp]):
        #     return

        # 计算距离
        distance = haversine(Config.B_POINT[0], Config.B_POINT[1], ship_info["Longitude"], ship_info["Latitude"])
        ship_info["Distance"] = distance

        # 筛选条件
        if distance <= Config.MAX_DISTANCE and ship_info["Speed"] > Config.MIN_SPEED:
            with ais_lock:
                ship_data_dict[mmsi] = ship_info  # 更新船舶信息
                # logging.info(f"Updated Ship Data: {ship_data_dict[mmsi]}")
        # print(ship_data_dict)
    except Exception as e:
        pass
        # logging.error(f"Error processing message: {e}")
def clean_expired_data():
    """定期清理过期数据"""
    while True:
        current_time = int(time.time())
        expired_mmsi = []

        with ais_lock:
            for mmsi, data in ship_data_dict.items():
                if current_time - data["Timestamp"] > Config.DATA_TIMEOUT:
                    expired_mmsi.append(mmsi)

            for mmsi in expired_mmsi:
                del ship_data_dict[mmsi]
                # logging.info(f"Removed expired data for MMSI: {mmsi}")

        time.sleep(Config.DATA_TIMEOUT)  # 每 DATA_TIMEOUT 秒检查一次

def get_ais_data():
    """获取当前 AIS 数据"""
    with ais_lock:
        return ship_data_dict.copy()

def start_mqtt_client():
    """启动 MQTT 客户端"""
    client = Client(client_id=Config.CLIENT_ID)
    client.username_pw_set(Config.USERNAME, Config.PASSWORD)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(Config.BROKER_HOST, Config.BROKER_PORT, 60)
    client.loop_forever()

def findname(mmsi):
    url = 'https://alpha.hifleet.com/hifleetapi/getShipAisNameByMmsis.do?mmsis='+str(mmsi)+'&usertoken=12'
    r = requests.get(url)
    dictinfo = json.loads(r.text)
    return dictinfo[0]['name']

def main():
    """主函数"""
    # 启动 MQTT 客户端线程
    mqtt_thread = threading.Thread(target=start_mqtt_client, daemon=True)
    mqtt_thread.start()

    # 启动数据清理线程
    clean_thread = threading.Thread(target=clean_expired_data, daemon=True)
    clean_thread.start()

    # 主线程保持运行
    while True:
        time.sleep(1)

if __name__ == '__main__':
    main()