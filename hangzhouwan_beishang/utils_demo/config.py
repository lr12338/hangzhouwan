from utils_demo.find_ship import (
    beixia_predict_longitude_latitude,
    beishang_predict_longitude_latitude
)

STREAM_CONFIGS = {
    "A": {
        "stream_id": "A",
        "stream_url": "rtsp://admin:Few181818@112.16.184.176:48554/streaming/Channels/801",#高清
        # "stream_url": "http://open.ys7.com/v3/openlive/FS5533687_8_1.m3u8?expire=1766651274&id=793878039684980736&t=04ee1ce4f559056f4d9cad47c07f948f4a55390c616dfa6f05ac6642c6e7756f&ev=100",#高清
        # "stream_url": "http://open.ys7.com/v3/openlive/FS5533687_8_2.m3u8?expire=1766651274&id=793878039371984896&t=57922a15105097d2b2221a981ca35d3a179818b8e3efa70f74aeef9cfd2c471e&ev=100",#流畅
        "rtmp_url": "rtmp://hangzhouwanpush.hifleet.com:1935/HangZhouBridge/HangZhouBridgeNorth8_1",
        "forbidden_rectangles": [
            ((1480, 0), (2560, 630)),
            ((247, 855), (275, 888)),
            ((2295, 895), (2315, 927))
        ],
        "forbidden_polygons": [
            [(0, 0), (0, 640), (710, 620), (1260, 620), (1260, 0)]
        ],
        "predict_coord_func": beixia_predict_longitude_latitude,
        "camera_param": 0.5
    },
    "B": {
        "stream_id": "B",
        "stream_url": "rtsp://admin:Few181818@112.16.184.176:48554/streaming/Channels/301",#高清
        # "stream_url": "http://open.ys7.com/v3/openlive/FS5533687_3_1.m3u8?expire=1766651012&id=793876943232565248&t=9a6f3793c6225fe7385e4a5e83299860bdaf464eb080db76d554f9a4b3d582e3&ev=100",#高清
        # "stream_url": "http://open.ys7.com/v3/openlive/FS5533687_3_2.m3u8?expire=1766651012&id=793876942885629952&t=38223391e1aff1a540843c46ae2db1d61d6f710e88f52487ca1b86d1df080a3b&ev=100",#流畅
        "rtmp_url": "rtmp://hangzhouwanpush.hifleet.com:1935/HangZhouBridge/HangZhouBridgeNorth3_1",
        "forbidden_rectangles": [
            ((0, 0), (2560, 210))
        ],
        "forbidden_polygons": [
            [(2160, 210), (2560, 280), (2560, 210)]
        ],
        "predict_coord_func": beishang_predict_longitude_latitude,
        "camera_param": 0.8
    }
}

ffmpeg_path = r"D:\\huangchao\\ffmpeg\\bin\\ffmpeg.exe"

font_path=r'D:\huangchao\hangzhouwan_beishang\weights\simhei.ttf'

detector_path = r'D:\huangchao\hangzhouwan_beishang\weights\best.onnx'

#预测位置
beixia_model_path=r'D:\huangchao\hangzhouwan_beishang\weights\0121_random_forest_model.pkl'
beishang_model_path=r'D:\huangchao\hangzhouwan_beishang\weights\beishang_x-l.pkl'