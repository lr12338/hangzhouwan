// -*- coding: utf-8 -*-
// =============================================================================
// Sophon-FFmpeg C API 兼容封装。
//
// 背景：Sophon-FFmpeg 0.8.0（基于 FFmpeg 4.1.3）仅提供运行时库，不含开发头文件。
// 解决：使用 Ubuntu focal 的 libavformat-dev 4.2.7 头文件（libavcodec 58、libavutil 56、
// libswscale 5，与 Sophon 4.1.3 库同 major 版本，ABI 兼容），安装到
// /opt/sophon/sophon-ffmpeg_0.8.0/include，链接 Sophon 运行时库。
//
// 注意：FFmpeg 4.x 公共头文件已移除 extern "C" 守卫，C++ 引用必须显式包裹，否则符号
// 会被 C++ name-mangle，链接时报 undefined reference。本头文件统一在此处理。
// =============================================================================
#ifndef HZW_VIDEO_FFMPEG_COMPAT_H
#define HZW_VIDEO_FFMPEG_COMPAT_H

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

#endif  // HZW_VIDEO_FFMPEG_COMPAT_H
