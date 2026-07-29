// -*- coding: utf-8 -*-
// =============================================================================
// SophonVideoSource：Sophon H.264 硬件解码视频源。
//
// 使用 Sophon-FFmpeg C API（avcodec + h264_bm）打开本地文件或 RTSP 流，硬件解码为
// NV12 帧。解码帧的 AVFrame 承载 bm_image，其 data[] 为 host 可读写的 mmap 设备内存。
//
// 两种输入：
//   - 本地文件（open）：支持 loop，N>0 播放 N 遍，0 表示无限循环直到外部停止。
//   - RTSP（open_rtsp）：实时流，不支持 loop/seek；av_read_frame 失败时按受控退避重连。
//
// 中断回调：fmt_->interrupt_callback 指向 stop_requested_，使 avformat_open_input 与
// av_read_frame 可被 request_stop()（信号/限时）及时中断，避免永久阻塞。
// =============================================================================
#ifndef HZW_VIDEO_VIDEO_SOURCE_H
#define HZW_VIDEO_VIDEO_SOURCE_H

#include <atomic>
#include <cstdint>
#include <string>
#include "video/ffmpeg_compat.h"
#include "video/rtsp_source_options.h"
#include "video/video_frame.h"
#include "video/inflight_frame_tracker.h"
#include <mutex>

namespace hzw {

class SophonVideoSource {
 public:
  SophonVideoSource() = default;
  ~SophonVideoSource();

  SophonVideoSource(const SophonVideoSource&) = delete;
  SophonVideoSource& operator=(const SophonVideoSource&) = delete;

  // 打开本地输入文件并初始化 h264_bm 解码器。extra_frame_buffer_num 控制解码输出缓冲池
  // （需 >= 编码器保留帧数，默认 20 以避免 bm_image 池死锁）。
  bool open(const std::string& path, int device, int extra_frame_buffer_num,
            const std::string& decoder_name, std::string& err);

  // 打开 RTSP 输入流并初始化 h264_bm 解码器。opts 控制传输/超时/重连。
  // URL 仅存于内存，日志中一律脱敏（见 redact_url_credentials）。
  bool open_rtsp(const std::string& url, int device, int extra_frame_buffer_num,
                 const std::string& decoder_name, const RtspSourceOptions& opts,
                 std::string& err);

  // 设置循环次数（0=无限）。仅本地文件有效；需在 open 之后、read 之前调用。
  void set_loop(int n) { loop_ = n; }

  // 请求停止：设置中断标志，使阻塞中的 open/read 尽快返回（信号/限时调用）。
  void request_stop() { stop_requested_.store(true); }

  // 读取一帧到 vf（所有权转移给调用者，需 vf.release()）。每帧的 frame_tracker_ 指向
  // 源在途计数器，release() 时递减。返回 false 时 err 给出原因：
  //   "EOF"           本地文件且 loop 已用尽
  //   "停止"           信号/限时主动停止
  //   "RTSP_RECONNECT" RTSP 断流，需由调用方（管线）清空缓冲、等待在途帧归零后再调
  //                    reconnect_rtsp()；read 自身不再重建解码器（避免在途帧未释放即关闭解码器）。
  //   其它            不可恢复错误（含 DEVICE_RESOURCE_FATAL：VPU/解码器资源耗尽）。
  bool read(VideoFrame& vf, std::string& err);

  int width() const { return width_; }
  int height() const { return height_; }
  int fps() const { return fps_; }
  AVRational time_base() const { return time_base_; }
  int64_t sequence() const { return seq_; }
  int64_t packets_read() const { return packets_read_; }
  int loops_completed() const { return loops_completed_; }
  bool is_rtsp() const { return is_rtsp_; }
  int reconnect_count() const { return reconnect_count_; }
  int64_t reconnect_attempt_count() const {
    return reconnect_attempt_count_.load();
  }
  int64_t reconnect_failure_count() const {
    return reconnect_failure_count_.load();
  }
  // 源 epoch：每次（重）连接成功后 +1；帧携带该值，用于检测重连并清除过期检测结果。
  int source_epoch() const { return source_epoch_; }

  // 在途 AVFrame 数（已 produce 未 release）。重连关闭旧解码器前必须等待其归零。
  int outstanding_avframes() const { return inflight_.count(); }
  // 阻塞等待在途帧归零；超时返回 false（未归零，禁止关闭解码器）。
  bool wait_avframes_drained(int timeout_ms) const { return inflight_.wait_drained(timeout_ms); }

  // 设备资源致命（VPU/解码器 ENOMEM 或初始化失败）。致命后管线必须停止双路并非零退出。
  bool resource_fatal() const { return resource_fatal_.load(); }
  std::string fatal_reason() const;

  // RTSP 受控重连（由管线在清空缓冲、等待在途帧归零后调用）：
  //   退避循环 -> 关闭旧解码器（仅在 outstanding_avframes()==0 时，否则升级致命）->
  //   重开 RTSP 输入 -> 重开解码器。成功后 epoch+1。
  // 不重新加载 bmodel，不触碰 RTMP/编码器。返回 false 表示停止、重连耗尽或资源致命。
  bool reconnect_rtsp(std::string& err);

  // 测试钩子：对当前源（文件或 RTSP）执行一次与重连等价的解码器换建
  // （等待在途帧归零 -> 关闭旧解码器 -> 重开输入 -> 重开解码器，epoch+1）。
  // 仅供板端 forced-reconnect 工具压测解码器生命周期，生产路径不调用。
  bool simulate_reconnect(std::string& err);

  void close();

 private:
  // 打开输入（文件或 RTSP）：预分配 ctx 并设置中断回调，再 avformat_open_input +
  // find_stream_info + 定位视频流 + 读取宽高/帧率/time_base。opts 由调用方构造（可空）。
  bool open_input(const std::string& url, AVDictionary** opts_ptr, std::string& err);
  bool open_decoder(const std::string& decoder_name, std::string& err);
  // 关闭解码器：硬性断言 outstanding_avframes()==0，未归零则升级 DEVICE_RESOURCE_FATAL
  // 并拒绝关闭（避免泄漏仍被在途帧引用的 bm_image 设备内存）。
  void close_decoder();
  bool seek_to_start(std::string& err);
  // 单次重开：关闭旧解码器（受在途帧保护）-> 关闭旧输入 -> 重开输入 -> 重开解码器。
  // 成功返回 true 并更新 epoch/reconnect_count。失败时已释放半初始化上下文。
  bool try_reopen(const std::string& url, const std::string& decoder_name,
                  AVDictionary** opts_ptr, std::string& err);
  // 标记设备资源致命（VPU/解码器 ENOMEM 或初始化失败）。
  void mark_resource_fatal(const std::string& reason);
  // 可被停止中断的睡眠（按 50ms 片段轮询 stop_requested_）。
  void sleep_interruptible(int64_t ms);
  static int interrupt_cb(void* opaque);

  AVFormatContext* fmt_ = nullptr;
  AVCodecContext* dec_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVFrame* frame_ = nullptr;
  int video_index_ = -1;
  int width_ = 0, height_ = 0, fps_ = 0;
  AVRational time_base_{1, 1};
  int64_t seq_ = 0;
  int64_t packets_read_ = 0;
  int loop_ = 1;                   // 0 = 无限（仅本地文件）
  int loops_completed_ = 0;
  int device_ = 0;
  int extra_frame_buffer_num_ = 20;
  std::string decoder_name_ = "h264_bm";
  std::string path_;

  // RTSP 状态
  std::atomic<bool> stop_requested_{false};
  bool is_rtsp_ = false;
  RtspSourceOptions rtsp_opts_;
  ReconnectPolicy reconnect_policy_{1000, 30000};
  int reconnect_count_ = 0;
  std::atomic<int64_t> reconnect_attempt_count_{0};
  std::atomic<int64_t> reconnect_failure_count_{0};
  int source_epoch_ = 0;
  std::string rtsp_url_;           // 仅内存，禁止打印

  // 资源生命周期状态
  InflightFrameTracker inflight_;            // 在途 AVFrame 计数（produce/release 配对）
  std::atomic<bool> resource_fatal_{false};  // 设备资源致命标志
  mutable std::mutex fatal_mutex_;
  std::string fatal_reason_;
};

}  // namespace hzw

#endif  // HZW_VIDEO_VIDEO_SOURCE_H
