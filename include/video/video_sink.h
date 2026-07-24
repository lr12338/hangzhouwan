// -*- coding: utf-8 -*-
// =============================================================================
// SophonVideoSink：Sophon H.264 硬件编码 + 封装输出。
//
// 使用 h264_bm 编码器。编码器要求输入 AVFrame 承载 bm_image（解码帧可直接复用），
// 其 NV12 像素经处理线程 in-place 修改后送入编码。封装优先 MP4，若编码器未提供
// extradata 或 MP4 封装失败，自动回退到 TS/裸 H.264。禁止使用 libx264。
//
// 阶段4.3：增加 RTMP 网络输出（FLV muxer + RTMP 协议）与重连状态机。
// RTMP 重连仅重建 FLV muxer + RTMP AVIO，保留硬件编码器（避免 VPU 显存 churn），
// 不重新加载 bmodel，不重新连接 RTSP；编码器 ENOMEM 才升级 DEVICE_RESOURCE_FATAL。
// =============================================================================
#ifndef HZW_VIDEO_VIDEO_SINK_H
#define HZW_VIDEO_VIDEO_SINK_H

#include <atomic>
#include <cstdint>
#include <string>
#include <atomic>
#include <mutex>
#include "video/ffmpeg_compat.h"
#include "video/video_frame.h"

namespace hzw {

// 输出 PTS 序列：按输出帧序严格递增分配 PTS，忽略源 PTS。
// 保证输出时间戳单调（源 PTS 回退/RTSP 重连跳变不影响输出），可独立单元测试。
// reset() 仅在「编码器同时重建」的重连路径合法（编码器 DTS 归零时 PTS 才可归零）。
// muxer-only 重连（编码器不重建）禁止调用 reset()，否则 PTS 归零 < 续接 DTS 致 FLV 永久写帧失败。
struct OutputPtsSequence {
  int64_t next() { return seq_++; }
  int64_t current() const { return seq_; }
  void reset() { seq_ = 0; }
 private:
  int64_t seq_ = 0;
};

// RTMP 退避策略：固定序列 1s, 2s, 5s, 10s，之后封顶 30s。
// 成功后重置。纯逻辑，不依赖网络/硬件/线程，可独立单元测试。
class RtmpBackoffPolicy {
 public:
  // 记录一次失败，返回下次应等待的退避毫秒。
  int64_t on_failure();
  // 成功后重置。
  void on_success();
  int attempts() const { return attempts_; }
  int64_t last_backoff_ms() const { return last_backoff_ms_; }

 private:
  int attempts_ = 0;
  int64_t last_backoff_ms_ = 0;
};

// RTMP 重连动作决策（纯逻辑，可独立单元测试，不依赖网络/硬件/FFmpeg 对象）：
//   普通网络写失败/连接断开 -> 仅重建 muxer/AVIO，保留硬件编码器；
//   编码器 ENOMEM 或编码器未打开 -> 升级 DEVICE_RESOURCE_FATAL，停止双路并非零退出。
// send_frame_err 为 avcodec_send_frame 返回码（0/负值）；encoder_open 表示编码器是否可用。
enum class RtmpReconnectAction {
  REBUILD_MUXER_ONLY,  // 网络层断开：仅重建 FLV muxer + RTMP AVIO
  ESCALATE_FATAL,      // 编码器/设备资源致命：停止并退出
};
RtmpReconnectAction decide_rtmp_reconnect(int send_frame_err, bool encoder_open);

class SophonVideoSink {
 public:
  SophonVideoSink() = default;
  ~SophonVideoSink();

  SophonVideoSink(const SophonVideoSink&) = delete;
  SophonVideoSink& operator=(const SophonVideoSink&) = delete;

  // 打开输出并初始化 h264_bm 编码器与封装器。
  // sink_type: "file"（本地文件）或 "rtmp"（RTMP 网络推流，使用 FLV muxer）。
  bool open(const std::string& path, int width, int height, int fps,
            int bitrate_kbps, int gop, int device,
            const std::string& encoder_name,
            const std::string& sink_type, std::string& err);

  // 写入一帧（按输出帧序重置 pts）。返回 false 表示不可恢复错误。
  // RTMP 模式下，写入失败会自动尝试重连（退避+重建编码器/muxer），重连成功后重试写入。
  bool write(VideoFrame& vf, std::string& err);

  // 刷新编码器并写 trailer。
  void close();

  // 请求停止（信号处理调用）：中断 RTMP 退避等待。
  void request_stop() { stop_requested_.store(true); }

  int64_t output_frames() const { return total_output_frames_.load(); }
  std::string actual_container() const { return container_; }
  std::string actual_encoder() const { return encoder_name_; }
  std::string sink_type() const { return sink_type_; }
  int rtmp_reconnect_count() const { return rtmp_reconnect_count_; }

  // 设备资源致命（编码器 ENOMEM 或初始化失败）。致命后管线必须停止双路并非零退出。
  bool resource_fatal() const { return resource_fatal_.load(); }
  std::string fatal_reason() const;

  // 测试钩子：强制一次“仅重建 muxer”的重连（复用现有编码器），供板端工具压测。
  // 需先 open() 成功；url 为 RTMP 地址。返回 false 表示停止或资源致命。
  bool simulate_rtmp_reconnect(const std::string& url, std::string& err);

 private:
  bool open_encoder(int width, int height, int fps, int bitrate_kbps, int gop,
                    int device, const std::string& encoder_name, std::string& err);
  bool open_file_muxer(const std::string& path, std::string& err);
  bool open_rtmp_muxer(const std::string& url, std::string& err);
  // 仅释放 muxer 与 AVIO（保留编码器），用于 RTMP 网络重连。
  void teardown_muxer();
  // 释放 muxer 与编码器（用于 close / 资源致命）。
  void teardown_muxer_encoder();
  // RTMP 重连：退避 -> 仅重建 FLV muxer + RTMP AVIO（保留 enc_ 硬件编码器）-> 重写 header。
  // 成功后重置 PTS（新会话从 0 开始，避免 non-monotonous DTS）。返回 false 表示停止或不可恢复。
  bool reconnect_rtmp(std::string& err);
  // 可被停止中断的睡眠。
  void sleep_interruptible(int64_t ms);
  void drain_packets(std::string& err);
  void mark_resource_fatal(const std::string& reason);

  AVFormatContext* mux_ = nullptr;      // 输出封装上下文
  AVCodecContext* enc_ = nullptr;       // h264_bm 编码器
  AVStream* vstream_ = nullptr;
  AVPacket* pkt_ = nullptr;
  OutputPtsSequence pts_;              // 输出帧序（用于 pts，严格递增）
  bool header_written_ = false;
  std::string container_;               // 实际容器：mp4/ts/h264/flv
  std::string encoder_name_;
  std::string sink_type_ = "file";      // file | rtmp

  // RTMP 重连状态
  std::atomic<bool> stop_requested_{false};
  RtmpBackoffPolicy rtmp_backoff_;
  int rtmp_reconnect_count_ = 0;
  std::atomic<int64_t> total_output_frames_{0};  // 实际写入总帧数（不受 PTS 重置影响）
  // 保存重连所需的参数
  std::string output_url_;
  int saved_width_ = 0;
  int saved_height_ = 0;
  int saved_fps_ = 0;
  int saved_bitrate_kbps_ = 0;
  int saved_gop_ = 0;
  int saved_device_ = 0;
  std::string saved_encoder_name_;

  // 资源致命状态
  std::atomic<bool> resource_fatal_{false};
  mutable std::mutex fatal_mutex_;
  std::string fatal_reason_;
};

}  // namespace hzw

#endif  // HZW_VIDEO_VIDEO_SINK_H
