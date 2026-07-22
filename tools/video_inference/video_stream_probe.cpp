// -*- coding: utf-8 -*-
// =============================================================================
// 视频流探测工具（阶段4.3 任务2）：探测 RTSP 流的真实编码、分辨率、帧率等参数。
//
// 使用 Sophon-FFmpeg 打开 RTSP 流，读取流信息并解码少量帧以验证可用性。
// 运行时间由 --max-seconds 控制（默认 20 秒）。
//
// 用法:
//   ./build/video_stream_probe --input 'rtsp://...' --max-seconds 20
// =============================================================================
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "video/ffmpeg_compat.h"
#include "video/rtsp_source_options.h"

namespace {
std::atomic<bool> g_stop{false};

int interrupt_cb(void* opaque) {
  auto* stop = static_cast<std::atomic<bool>*>(opaque);
  return (stop && stop->load()) ? 1 : 0;
}

void on_signal(int sig) {
  (void)sig;
  g_stop.store(true);
  std::fprintf(stdout, "\n信息 | 信号 | 收到信号，请求停止\n");
  std::fflush(stdout);
}

const char* pixfmt_name(int fmt) {
  switch (fmt) {
    case AV_PIX_FMT_NV12: return "nv12";
    case AV_PIX_FMT_YUV420P: return "yuv420p";
    case AV_PIX_FMT_YUVJ420P: return "yuvj420p";
    default: return av_get_pix_fmt_name(static_cast<AVPixelFormat>(fmt));
  }
}
}  // namespace

int main(int argc, char** argv) {
  std::string input;
  std::string transport = "tcp";
  int64_t stimeout_us = 5000000;
  int max_seconds = 20;
  bool help = false;

  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    if (k == "--help" || k == "-h") { help = true; break; }
    else if (k == "--input" && i + 1 < argc) input = argv[++i];
    else if (k == "--transport" && i + 1 < argc) transport = argv[++i];
    else if (k == "--stimeout-us" && i + 1 < argc) stimeout_us = std::strtoll(argv[++i], nullptr, 10);
    else if (k == "--max-seconds" && i + 1 < argc) max_seconds = std::atoi(argv[++i]);
    else { std::fprintf(stderr, "未知参数: %s\n", k.c_str()); return 1; }
  }
  if (help || input.empty()) {
    std::fprintf(stdout, "用法: video_stream_probe --input <rtsp-url> [--transport tcp] [--max-seconds 20]\n");
    return help ? 0 : 1;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  AVFormatContext* fmt = avformat_alloc_context();
  fmt->interrupt_callback.callback = interrupt_cb;
  fmt->interrupt_callback.opaque = &g_stop;

  auto t_start = std::chrono::steady_clock::now();

  AVDictionary* dict = nullptr;
  av_dict_set(&dict, "rtsp_transport", transport.c_str(), 0);
  if (stimeout_us > 0) av_dict_set_int(&dict, "stimeout", stimeout_us, 0);

  int r = avformat_open_input(&fmt, input.c_str(), nullptr, &dict);
  av_dict_free(&dict);
  if (r < 0 || !fmt) {
    char eb[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(r < 0 ? r : AVERROR_UNKNOWN, eb, sizeof(eb));
    std::fprintf(stderr, "错误 | 探测 | 打开输入失败: %s\n", eb);
    return 1;
  }
  auto t_connected = std::chrono::steady_clock::now();
  int64_t connect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_connected - t_start).count();
  std::fprintf(stdout, "信息 | 探测 | RTSP 连接成功，握手耗时 %lldms\n",
               static_cast<long long>(connect_ms));

  if (avformat_find_stream_info(fmt, nullptr) < 0) {
    std::fprintf(stderr, "错误 | 探测 | 无法获取流信息\n");
    avformat_close_input(&fmt);
    return 1;
  }

  int video_index = -1;
  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_index = static_cast<int>(i);
      break;
    }
  }
  if (video_index < 0) {
    std::fprintf(stderr, "错误 | 探测 | 未找到视频流\n");
    avformat_close_input(&fmt);
    return 1;
  }

  AVStream* st = fmt->streams[video_index];
  AVCodecParameters* par = st->codecpar;
  const AVCodec* dec = avcodec_find_decoder(par->codec_id);
  AVRational fr = st->avg_frame_rate;
  if (fr.num <= 0 || fr.den <= 0) fr = st->r_frame_rate;
  int fps = (fr.num > 0 && fr.den > 0) ? static_cast<int>((fr.num + fr.den / 2) / fr.den) : 0;
  const char* profile_name = nullptr; // av_profile_name 在此 FFmpeg 版本不可用

  std::fprintf(stdout, "信息 | 探测 | 编码格式: %s (codec_id=%d)\n",
               dec ? dec->name : "unknown", par->codec_id);
  std::fprintf(stdout, "信息 | 探测 | 分辨率: %dx%d\n", par->width, par->height);
  std::fprintf(stdout, "信息 | 探测 | 帧率: %dfps\n", fps);
  std::fprintf(stdout, "信息 | 探测 | profile: %d\n", par->profile);
  std::fprintf(stdout, "信息 | 探测 | pixel_format: %s\n", pixfmt_name(par->format));
  std::fprintf(stdout, "信息 | 探测 | time_base: %d/%d\n", st->time_base.num, st->time_base.den);

  // 尝试解码少量帧验证可用性
  AVCodecContext* dec_ctx = avcodec_alloc_context3(dec);
  avcodec_parameters_to_context(dec_ctx, par);
  av_opt_set_int(dec_ctx, "sophon_idx", 0, 0);
  AVDictionary* opts = nullptr;
  av_dict_set_int(&opts, "extra_frame_buffer_num", 20, 0);
  r = avcodec_open2(dec_ctx, dec, &opts);
  av_dict_free(&opts);
  if (r < 0) {
    char eb[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(r, eb, sizeof(eb));
    std::fprintf(stdout, "警告 | 探测 | 解码器打开失败: %s（仅流信息可用）\n", eb);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt);
    return 0;
  }

  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  int decoded_frames = 0;
  int64_t first_frame_ms = -1;
  auto t_read_start = std::chrono::steady_clock::now();

  while (!g_stop.load() && decoded_frames < 60) {
    auto t_now = std::chrono::steady_clock::now();
    int64_t elapsed = std::chrono::duration_cast<std::chrono::seconds>(t_now - t_start).count();
    if (elapsed >= max_seconds) break;

    int rr = av_read_frame(fmt, pkt);
    if (rr < 0) break;
    if (pkt->stream_index == video_index) {
      avcodec_send_packet(dec_ctx, pkt);
    }
    av_packet_unref(pkt);

    while (true) {
      int ret = avcodec_receive_frame(dec_ctx, frame);
      if (ret == 0) {
        if (first_frame_ms < 0) {
          auto t_ff = std::chrono::steady_clock::now();
          first_frame_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_ff - t_read_start).count();
        }
        ++decoded_frames;
        av_frame_unref(frame);
      } else {
        break;
      }
    }
  }

  auto t_end = std::chrono::steady_clock::now();
  int64_t total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_read_start).count();

  std::fprintf(stdout, "信息 | 探测 | 首帧耗时: %lldms\n", static_cast<long long>(first_frame_ms));
  std::fprintf(stdout, "信息 | 探测 | %lldms 内解码 %d 帧", static_cast<long long>(total_ms), decoded_frames);
  if (total_ms > 0) {
    std::fprintf(stdout, "（约 %.1ffps）", decoded_frames * 1000.0 / total_ms);
  }
  std::fprintf(stdout, "\n");

  // 推荐解码器
  if (dec) {
    std::string dec_name = dec->name;
    if (dec_name == "h264_bm") {
      std::fprintf(stdout, "信息 | 探测 | 推荐解码器: h264_bm\n");
    } else if (dec_name == "h265_bm" || dec_name == "hevc_bm") {
      std::fprintf(stdout, "信息 | 探测 | 推荐解码器: h265_bm\n");
    } else {
      std::fprintf(stdout, "信息 | 探测 | 当前解码器: %s（如需硬件解码请确认 h264_bm/h265_bm 可用）\n",
                   dec_name.c_str());
    }
  }

  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&dec_ctx);
  avformat_close_input(&fmt);
  std::fflush(stdout);
  return 0;
}
