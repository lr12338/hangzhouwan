# -*- coding: utf-8 -*-
# =============================================================================
# 阶段3 单图 C++ 推理 PoC 构建配置。
# 依赖：板端 libsophon 0.4.9（开发头文件）+ libjpeg-turbo（JPEG 读写）。
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

find_package(JPEG REQUIRED)  # libjpeg-turbo

#BMRT 运行时库
add_library(bmrt_runtime INTERFACE IMPORTED)
set_target_properties(bmrt_runtime PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${LIBSOPHON_INCLUDE}"
  INTERFACE_LINK_DIRECTORIES "${LIBSOPHON_LIB}")
target_link_libraries(bmrt_runtime INTERFACE bmrt bmlib pthread dl)

# 推理/图像/工具静态库
add_library(hzw_inf STATIC
  src/inference/bmrt_detector.cpp
  src/inference/yolov7_postprocess.cpp
  src/image_io/jpeg_io.cpp
  src/util/sha256.cpp)
target_include_directories(hzw_inf PUBLIC include ${LIBSOPHON_INCLUDE})
target_link_libraries(hzw_inf PUBLIC bmrt_runtime JPEG::JPEG)

# 单图推理 PoC
add_executable(single_image_infer tools/image_inference/single_image_infer.cpp)
target_link_libraries(single_image_infer PRIVATE hzw_inf)

# YOLOv7 后处理单元测试
enable_testing()
add_executable(test_postprocess tests/unit_cpp/test_yolov7_postprocess.cpp)
target_link_libraries(test_postprocess PRIVATE hzw_inf)
add_test(NAME postprocess COMMAND test_postprocess)

# 阶段2 门禁：最小 BMRuntime 加载验证
add_executable(bmrt_load_test tools/image_inference/bmrt_load_test.cpp)
target_link_libraries(bmrt_load_test PRIVATE bmrt_runtime)
