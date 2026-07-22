# -*- coding: utf-8 -*-
# =============================================================================
# BM1684 边缘推理构建配置。
# 依赖：libsophon 0.4.9（BMCV/BMRuntime）+ Sophon-FFmpeg 0.8.0（h264_bm 编解码）
#       + libjpeg-turbo（JPEG 读写）。
# 用法：
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#   ctest --test-dir build --output-on-failure
# =============================================================================
cmake_minimum_required(VERSION 3.10)
project(hangzhouwan_edge_inference LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# libsophon 路径（可覆盖）
set(LIBSOPHON_ROOT "/opt/sophon/libsophon-0.4.9" CACHE PATH "libsophon 安装路径")
set(LIBSOPHON_INCLUDE "${LIBSOPHON_ROOT}/include")
set(LIBSOPHON_LIB "${LIBSOPHON_ROOT}/lib")

# Sophon-FFmpeg 路径（运行时库 + 经 Ubuntu 4.2.7 dev 头文件补齐的开发头文件）
set(SOPHON_FFMPEG_ROOT "/opt/sophon/sophon-ffmpeg_0.8.0" CACHE PATH "Sophon-FFmpeg 安装路径")
set(SOPHON_FFMPEG_INCLUDE "${SOPHON_FFMPEG_ROOT}/include")
set(SOPHON_FFMPEG_LIB "${SOPHON_FFMPEG_ROOT}/lib")

find_package(JPEG REQUIRED)  # libjpeg-turbo
find_package(Threads REQUIRED)

# BMRuntime 运行时库
add_library(bmrt_runtime INTERFACE IMPORTED)
set_target_properties(bmrt_runtime PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${LIBSOPHON_INCLUDE}"
  INTERFACE_LINK_DIRECTORIES "${LIBSOPHON_LIB}")
target_link_libraries(bmrt_runtime INTERFACE bmrt bmlib bmcv pthread dl)

# Sophon-FFmpeg（avformat/avcodec/swscale/avutil，C++ 需 extern "C" 包裹头文件）
add_library(sophon_ffmpeg INTERFACE IMPORTED)
set_target_properties(sophon_ffmpeg PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${SOPHON_FFMPEG_INCLUDE}"
  INTERFACE_LINK_DIRECTORIES "${SOPHON_FFMPEG_LIB}")
target_link_libraries(sophon_ffmpeg INTERFACE avformat avcodec swscale avutil)

# 推理/图像/视频/管线/指标静态库
add_library(hzw_inf STATIC
  src/inference/bmrt_detector.cpp
  src/inference/yolov7_postprocess.cpp
  src/image_io/jpeg_io.cpp
  src/util/sha256.cpp
  src/video/sophon_ffmpeg_source.cpp
  src/video/sophon_ffmpeg_sink.cpp
  src/video/rtsp_source_options.cpp
  src/video/bmcv_processor.cpp
  src/video/detection_region_filter.cpp
  src/config/stream_profile.cpp
  src/monitoring/pipeline_metrics.cpp
  src/pipeline/single_stream_pipeline.cpp)
target_include_directories(hzw_inf PUBLIC include ${LIBSOPHON_INCLUDE} ${SOPHON_FFMPEG_INCLUDE})
target_link_libraries(hzw_inf PUBLIC bmrt_runtime sophon_ffmpeg JPEG::JPEG Threads::Threads)

# 单图推理 PoC
add_executable(single_image_infer tools/image_inference/single_image_infer.cpp)
target_link_libraries(single_image_infer PRIVATE hzw_inf)

# 阶段4 单路视频硬件推理
add_executable(single_video_infer tools/video_inference/single_video_infer.cpp)
target_link_libraries(single_video_infer PRIVATE hzw_inf)

# CPU/BMCV 预处理正确性对照工具
add_executable(preprocess_compare tools/video_inference/preprocess_compare.cpp)
target_link_libraries(preprocess_compare PRIVATE hzw_inf)

# 阶段4.3 视频流探测工具
add_executable(video_stream_probe tools/video_inference/video_stream_probe.cpp)
target_link_libraries(video_stream_probe PRIVATE hzw_inf)

# YOLOv7 后处理单元测试
enable_testing()
add_executable(test_postprocess tests/unit_cpp/test_yolov7_postprocess.cpp)
target_link_libraries(test_postprocess PRIVATE hzw_inf)
add_test(NAME postprocess COMMAND test_postprocess)

# 阶段4 视频管线单元测试
add_executable(test_latest_frame_queue tests/unit_cpp/test_latest_frame_queue.cpp)
target_link_libraries(test_latest_frame_queue PRIVATE hzw_inf Threads::Threads)
add_test(NAME latest_frame_queue COMMAND test_latest_frame_queue)

add_executable(test_detection_snapshot tests/unit_cpp/test_detection_snapshot.cpp)
target_link_libraries(test_detection_snapshot PRIVATE hzw_inf)
add_test(NAME detection_snapshot COMMAND test_detection_snapshot)

add_executable(test_pipeline_timing tests/unit_cpp/test_pipeline_timing.cpp)
target_link_libraries(test_pipeline_timing PRIVATE hzw_inf)
add_test(NAME pipeline_timing COMMAND test_pipeline_timing)

# 阶段4 BMCV 预处理/绘制单元测试（需 BM1684 硬件）
add_executable(test_bmcv_processor tests/unit_cpp/test_bmcv_processor.cpp)
target_link_libraries(test_bmcv_processor PRIVATE hzw_inf)
add_test(NAME bmcv_processor COMMAND test_bmcv_processor)

# 阶段4.2 RTSP 源选项与输出 PTS 单元测试（纯逻辑，无需硬件）
add_executable(test_rtsp_source_options tests/unit_cpp/test_rtsp_source_options.cpp)
target_link_libraries(test_rtsp_source_options PRIVATE hzw_inf)
add_test(NAME rtsp_source_options COMMAND test_rtsp_source_options)

add_executable(test_output_pts tests/unit_cpp/test_output_pts.cpp)
target_link_libraries(test_output_pts PRIVATE hzw_inf)
add_test(NAME output_pts COMMAND test_output_pts)


# 阶段4.3 RTMP 退避策略单元测试（纯逻辑，无需硬件）
add_executable(test_rtmp_backoff tests/unit_cpp/test_rtmp_backoff.cpp)
target_link_libraries(test_rtmp_backoff PRIVATE hzw_inf)
add_test(NAME rtmp_backoff COMMAND test_rtmp_backoff)

# 阶段4.3 禁区过滤单元测试（纯逻辑，无需硬件）
add_executable(test_detection_region_filter tests/unit_cpp/test_detection_region_filter.cpp)
target_link_libraries(test_detection_region_filter PRIVATE hzw_inf)
add_test(NAME detection_region_filter COMMAND test_detection_region_filter)

# 阶段4.3 时间调度单元测试（纯逻辑，无需硬件）
add_executable(test_time_scheduling tests/unit_cpp/test_time_scheduling.cpp)
target_link_libraries(test_time_scheduling PRIVATE hzw_inf)
add_test(NAME time_scheduling COMMAND test_time_scheduling)

# 阶段4.3 StreamProfile 解析单元测试（纯逻辑，无需硬件）
add_executable(test_stream_profile tests/unit_cpp/test_stream_profile.cpp)
target_link_libraries(test_stream_profile PRIVATE hzw_inf)
add_test(NAME stream_profile COMMAND test_stream_profile)

# 稳定性测试脚本行为验证
add_test(NAME stability_script COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_stability_script.sh)

# 阶段2 门禁：最小 BMRuntime 加载验证
add_executable(bmrt_load_test tools/image_inference/bmrt_load_test.cpp)
target_link_libraries(bmrt_load_test PRIVATE bmrt_runtime)
