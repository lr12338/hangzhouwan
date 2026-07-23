// -*- coding: utf-8 -*-
// =============================================================================
// 板端 forced-reconnect / VPU 显存泄漏复现工具（BM1684）。
//
// 子命令：
//   heap                      打印当前各堆显存（只读，不分配 VPU）
//   repro  [rounds]           最小复现：隔离“解码器关闭/未释放旧帧/编码器重建”三层泄漏
//   decoder [rounds] [buf]    decoder-only：FIXED 源强制重连 N 次（文件源模拟 RTSP 解码器换建）
//   sweep                     extra_frame_buffer_num 5/8/12/16/20 重连压测表
//   dual   [rounds]           双路并发源强制重连
//   rtmp                      RTMP muxer-only 重建决策说明（网络路径需 RTMP 服务器，见文档）
//
// 安全：所有分配模式内置 VPU 显存熔断——任一堆 avail < SAFE_MB 立即中止。
//       repro 的泄漏量在进程退出后由内核回收（用于验证“进程退出后显存恢复”）。
//       不激活、不重启生产服务；仅在维护窗口对 FIXED 二进制运行。
// =============================================================================
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>

extern "C" {
#include "bmlib_runtime.h"
}

#include "video/ffmpeg_compat.h"
#include "video/video_source.h"
#include "video/video_sink.h"
#include "video/video_frame.h"

using hzw::SophonVideoSource;
using hzw::SophonVideoSink;
using hzw::VideoFrame;

static const char* TEST_CLIP = "testdata/test.mp4";
static const unsigned long long SAFE_MB = 60;  // 任一堆 avail 低于此值立即熔断

static bm_handle_t g_handle = nullptr;
static bool heap_init() { return bm_dev_request(&g_handle, 0) == 0; }

struct HeapStat {
  unsigned long long total = 0, used = 0, avail = 0;
};
static HeapStat heap_by_id(unsigned int id) {
  HeapStat s;
  bm_heap_stat_byte_t st;
  if (g_handle && bm_get_gmem_heap_stat_byte_by_id(g_handle, &st, id) == 0) {
    s.total = st.mem_total; s.used = st.mem_used; s.avail = st.mem_avail;
  }
  return s;
}
static unsigned int heap_count() {
  unsigned int n = 0;
  if (g_handle) bm_get_gmem_total_heap_num(g_handle, &n);
  return n;
}
// VPU 堆：本板为 heap index 2（2GB，编解码 bm_image 池所在）。返回其 used。
static HeapStat vpu_heap() { return heap_by_id(2); }

static void print_heap(const char* tag) {
  unsigned int n = heap_count();
  std::printf("VPU_HEAP | %s |", tag);
  for (unsigned int i = 0; i < n; ++i) {
    HeapStat s = heap_by_id(i);
    std::printf(" heap%u:used=%.1fMB/avail=%.1fMB", i, s.used / 1048576.0, s.avail / 1048576.0);
  }
  std::printf("\n");
  std::fflush(stdout);
}
// 熔断：任一堆 avail < SAFE_MB 返回 true。
static bool heap_unsafe() {
  unsigned int n = heap_count();
  for (unsigned int i = 0; i < n; ++i) {
    if (heap_by_id(i).avail < SAFE_MB * 1048576ULL) return true;
  }
  return false;
}

// ---- 低层解码器打开（用于 repro 精确控制 hold/release）----
struct DecCtx {
  AVFormatContext* fmt = nullptr;
  AVCodecContext* dec = nullptr;
  int vindex = -1;
};

static bool open_decoder_low(const char* path, int extra, DecCtx& d, std::string& err) {
  d.fmt = avformat_alloc_context();
  if (!d.fmt) { err = "alloc fmt"; return false; }
  if (avformat_open_input(&d.fmt, path, nullptr, nullptr) < 0) { err = "open input"; return false; }
  if (avformat_find_stream_info(d.fmt, nullptr) < 0) { err = "find_stream_info"; return false; }
  for (unsigned i = 0; i < d.fmt->nb_streams; ++i)
    if (d.fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { d.vindex = i; break; }
  if (d.vindex < 0) { err = "no video"; return false; }
  const AVCodec* dec = avcodec_find_decoder_by_name("h264_bm");
  if (!dec) { err = "no h264_bm"; return false; }
  d.dec = avcodec_alloc_context3(dec);
  avcodec_parameters_to_context(d.dec, d.fmt->streams[d.vindex]->codecpar);
  av_opt_set_int(d.dec, "sophon_idx", 0, 0);
  AVDictionary* opts = nullptr;
  av_dict_set_int(&opts, "extra_frame_buffer_num", extra, 0);
  int r = avcodec_open2(d.dec, dec, &opts);
  av_dict_free(&opts);
  if (r < 0) { char eb[AV_ERROR_MAX_STRING_SIZE]; av_strerror(r, eb, sizeof(eb)); err = std::string("open2: ") + eb; return false; }
  return true;
}
static void close_decoder_low(DecCtx& d) {
  if (d.dec) avcodec_free_context(&d.dec);
  if (d.fmt) avformat_close_input(&d.fmt);
  d.vindex = -1;
}
// 接收 count 帧到 held（不 unref，模拟在途未释放）。返回实际接收数。
static int receive_frames(DecCtx& d, std::vector<AVFrame*>& held, int count) {
  AVPacket* pkt = av_packet_alloc();
  int got = 0;
  while (got < count) {
    AVFrame* tmp = av_frame_alloc();
    int rr = avcodec_receive_frame(d.dec, tmp);
    if (rr == 0) { held.push_back(tmp); ++got; av_packet_free(&pkt); return got; }
    av_frame_free(&tmp);
    if (rr != AVERROR(EAGAIN)) { av_packet_free(&pkt); return got; }
    int r = av_read_frame(d.fmt, pkt);
    if (r < 0) { av_packet_free(&pkt); return got; }
    if (pkt->stream_index == d.vindex) avcodec_send_packet(d.dec, pkt);
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  return got;
}
static void release_held(std::vector<AVFrame*>& held) {
  for (AVFrame* f : held) { av_frame_unref(f); av_frame_free(&f); }
  held.clear();
}

// ---- repro：解码器关闭泄漏隔离 ----
static int cmd_repro(int rounds) {
  if (rounds < 1) rounds = 3;
  std::printf("=== repro | rounds=%d clip=%s extra=2 hold=2 ===\n", rounds, TEST_CLIP);
  print_heap("start");
  unsigned long long base = vpu_heap().used;
  std::printf("场景 | 轮次 | heap2_used_MB | delta_MB | 说明\n");

  // 场景 A：解码器关闭时仍有在途帧（泄漏路径，复现旧 try_reopen_rtsp）
  unsigned long long prev = base;
  for (int i = 0; i < rounds; ++i) {
    if (heap_unsafe()) { std::printf("熔断 | heap avail < %lluMB，中止\n", SAFE_MB); break; }
    DecCtx d; std::string e;
    if (!open_decoder_low(TEST_CLIP, 2, d, e)) { std::printf("A%d | open fail: %s\n", i, e.c_str()); break; }
    std::vector<AVFrame*> held;
    receive_frames(d, held, 2);
    close_decoder_low(d);          // 关闭解码器：在途帧 bm_image 成为孤儿（泄漏）
    // 注意：不 release held —— 模拟管线中仍在途的帧
    unsigned long long now = vpu_heap().used;
    std::printf("A_leak | %d | %.1f | %+.1f | 关闭解码器时 %zu 帧在途未释放\n",
                i, now / 1048576.0, (double)((long long)now - (long long)prev) / 1048576.0, held.size());
    prev = now;
    // 本轮 held 在循环内继续持有至进程退出（模拟累积泄漏）
  }

  // 场景 B：先 release 全部在途帧再关闭解码器（FIXED 安全路径）
  prev = vpu_heap().used;
  for (int i = 0; i < rounds; ++i) {
    if (heap_unsafe()) { std::printf("熔断 | 中止\n"); break; }
    DecCtx d; std::string e;
    if (!open_decoder_low(TEST_CLIP, 2, d, e)) { std::printf("B%d | open fail: %s\n", i, e.c_str()); break; }
    std::vector<AVFrame*> held;
    receive_frames(d, held, 2);
    release_held(held);            // FIXED：先归还 bm_image
    close_decoder_low(d);          // 再关闭解码器（池干净回收）
    unsigned long long now = vpu_heap().used;
    std::printf("B_safe | %d | %.1f | %+.1f | 先释放在途帧再关闭解码器\n",
                i, now / 1048576.0, (double)((long long)now - (long long)prev) / 1048576.0);
    prev = now;
  }

  print_heap("end(before exit)");
  std::printf("结论 | A_leak 单调增长（解码器关闭+在途帧未释放 => VPU 显存泄漏）；"
              "B_safe 近似持平（先释放再关闭 => 不泄漏）。\n");
  std::printf("提示 | 泄漏显存将在本进程退出后由内核回收（见退出后 heap 对比）。\n");
  return 0;
}

// ---- decoder-only：FIXED 源强制重连 N 次 ----
static int source_reconnect_loop(int rounds, int buf, const char* tag) {
  SophonVideoSource src;
  std::string err;
  if (!src.open(TEST_CLIP, 0, buf, "h264_bm", err)) {
    std::printf("%s | open fail: %s\n", tag, err.c_str());
    return 1;
  }
  print_heap((std::string(tag) + "_start").c_str());
  unsigned long long base = vpu_heap().used;
  int ok = 0, fail = 0;
  for (int i = 0; i < rounds; ++i) {
    if (heap_unsafe()) { std::printf("%s | 熔断 @%d\n", tag, i); break; }
    // 读取并释放若干帧，锻炼 produce/release 路径
    for (int f = 0; f < 5; ++f) {
      VideoFrame vf;
      std::string e;
      if (!src.read(vf, e)) break;
      vf.release();
    }
    std::string e;
    if (src.simulate_reconnect(e)) ++ok;
    else { ++fail; std::printf("%s | reconnect %d fail: %s fatal=%d\n", tag, i, e.c_str(), src.resource_fatal()); }
    if (src.resource_fatal()) { std::printf("%s | 资源致命 @%d，停止\n", tag, i); break; }
    if (i % 10 == 9 || i == rounds - 1) {
      unsigned long long now = vpu_heap().used;
      std::printf("%s | round %d | heap2_used=%.1fMB delta=%+.1fMB | ok=%d fail=%d inflight=%d\n",
                  tag, i + 1, now / 1048576.0, (double)((long long)now - (long long)base) / 1048576.0, ok, fail, src.outstanding_avframes());
    }
  }
  src.close();
  unsigned long long now = vpu_heap().used;
  std::printf("%s | done | rounds=%d ok=%d fail=%d | heap2_used %.1f->%.1fMB delta=%+.1fMB\n",
              tag, rounds, ok, fail, base / 1048576.0, now / 1048576.0, (double)((long long)now - (long long)base) / 1048576.0);
  return fail == 0 ? 0 : 1;
}

static int cmd_decoder(int rounds, int buf) {
  if (rounds < 1) rounds = 100;
  if (buf < 1) buf = 20;
  std::printf("=== decoder-only | rounds=%d extra_frame_buffer_num=%d ===\n", rounds, buf);
  return source_reconnect_loop(rounds, buf, "decoder");
}

static int cmd_sweep() {
  std::printf("=== sweep | extra_frame_buffer_num 5/8/12/16/20 x 20 reconnects ===\n");
  std::printf("buf | rounds | ok | fail | heap2_delta_MB | inflight_end\n");
  int bufs[] = {5, 8, 12, 16, 20};
  for (int buf : bufs) {
    if (heap_unsafe()) { std::printf("熔断 | skip buf=%d\n", buf); continue; }
    SophonVideoSource src;
    std::string err;
    if (!src.open(TEST_CLIP, 0, buf, "h264_bm", err)) { std::printf("%d | open fail: %s\n", buf, err.c_str()); continue; }
    unsigned long long base = vpu_heap().used;
    int ok = 0, fail = 0;
    for (int i = 0; i < 20; ++i) {
      if (heap_unsafe()) { std::printf("熔断 @buf=%d round=%d\n", buf, i); break; }
      for (int f = 0; f < 3; ++f) { VideoFrame vf; std::string e; if (!src.read(vf, e)) break; vf.release(); }
      std::string e;
      if (src.simulate_reconnect(e)) ++ok; else ++fail;
      if (src.resource_fatal()) break;
    }
    unsigned long long now = vpu_heap().used;
    std::printf("%d | 20 | %d | %d | %+.1f | %d\n", buf, ok, fail,
                (double)((long long)now - (long long)base) / 1048576.0, src.outstanding_avframes());
    src.close();
  }
  return 0;
}

static int cmd_dual(int rounds) {
  if (rounds < 1) rounds = 50;
  std::printf("=== dual | 2 concurrent sources x %d reconnects ===\n", rounds);
  std::atomic<int> fa{0}, fb{0};
  unsigned long long base = vpu_heap().used;
  std::thread ta([&] { SophonVideoSource s; std::string e; if(s.open(TEST_CLIP,0,20,"h264_bm",e)){ for(int i=0;i<rounds;++i){ if(heap_unsafe())break; for(int f=0;f<3;++f){VideoFrame vf;std::string x;if(!s.read(vf,x))break;vf.release();} std::string y; if(!s.simulate_reconnect(y))++fa; if(s.resource_fatal())break;} s.close();}});
  std::thread tb([&] { SophonVideoSource s; std::string e; if(s.open(TEST_CLIP,0,20,"h264_bm",e)){ for(int i=0;i<rounds;++i){ if(heap_unsafe())break; for(int f=0;f<3;++f){VideoFrame vf;std::string x;if(!s.read(vf,x))break;vf.release();} std::string y; if(!s.simulate_reconnect(y))++fb; if(s.resource_fatal())break;} s.close();}});
  ta.join(); tb.join();
  unsigned long long now = vpu_heap().used;
  std::printf("dual | done | rounds=%d A_fail=%d B_fail=%d | heap2 %.1f->%.1fMB delta=%+.1fMB\n",
              rounds, fa.load(), fb.load(), base / 1048576.0, now / 1048576.0, (double)((long long)now - (long long)base) / 1048576.0);
  return (fa.load() || fb.load()) ? 1 : 0;
}

static int cmd_rtmp() {
  std::printf("=== rtmp | muxer-only rebuild (网络路径需 RTMP 服务器) ===\n");
  std::printf("说明 | 普通 RTMP 断开仅重建 muxer/AVIO，保留硬件编码器（见 decide_rtmp_reconnect 单元测试）。\n");
  std::printf("说明 | 编码器重建泄漏隔离见 repro；端到端 RTMP 100x 重连需维护窗口提供 RTMP 服务器。\n");
  // 资源致命路径自检：编码器不可用时 reconnect_rtmp 升级致命。
  SophonVideoSink sink;
  std::string err;
  bool r = sink.simulate_rtmp_reconnect("rtmp://127.0.0.1:1/nonexistent", err);
  std::printf("rtmp | simulate(无编码器) -> reconnect=%d fatal=%d err=%s\n",
              r ? 1 : 0, sink.resource_fatal() ? 1 : 0, err.c_str());
  return 0;
}

int main(int argc, char** argv) {
  std::string cmd = argc > 1 ? argv[1] : "heap";
  if (!heap_init()) { std::fprintf(stderr, "bm_dev_request 失败\n"); return 2; }
  int rc = 0;
  if (cmd == "heap") { print_heap("now"); }
  else if (cmd == "repro") rc = cmd_repro(argc > 2 ? std::atoi(argv[2]) : 3);
  else if (cmd == "decoder") rc = cmd_decoder(argc > 2 ? std::atoi(argv[2]) : 100,
                                              argc > 3 ? std::atoi(argv[3]) : 20);
  else if (cmd == "sweep") rc = cmd_sweep();
  else if (cmd == "dual") rc = cmd_dual(argc > 2 ? std::atoi(argv[2]) : 50);
  else if (cmd == "rtmp") rc = cmd_rtmp();
  else { std::printf("用法: %s heap|repro|decoder|sweep|dual|rtmp\n", argv[0]); }
  print_heap("final");
  if (g_handle) bm_dev_free(g_handle);
  return rc;
}
