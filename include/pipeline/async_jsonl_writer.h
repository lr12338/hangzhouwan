// -*- coding: utf-8 -*-
#ifndef HZW_PIPELINE_ASYNC_JSONL_WRITER_H
#define HZW_PIPELINE_ASYNC_JSONL_WRITER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace hzw {

struct AsyncJsonlWriterConfig {
  std::string directory = "/data/hangzhouwan/events";
  std::string stream_id;
  int max_size_mb = 50;
  int rotate_seconds = 3600;
  int retention_days = 7;
  int sync_seconds = 5;
  int queue_max_mb = 4;
  int disk_warn_mb = 2048;
  int disk_stop_mb = 1024;
};

struct AsyncJsonlWriterStatus {
  bool running = false;
  bool storage_available = false;
  bool write_enabled = false;
  bool low_space_warning = false;
  int64_t free_bytes = 0;
  int64_t queued_bytes = 0;
  int64_t written_records = 0;
  int64_t dropped_records = 0;
  std::string active_path;
  std::string last_error;
};

// 有界、异步、仅写 /data 的 JSONL 写入器。生产视频线程只做内存入队。
class AsyncJsonlWriter {
 public:
  AsyncJsonlWriter() = default;
  ~AsyncJsonlWriter();

  AsyncJsonlWriter(const AsyncJsonlWriter&) = delete;
  AsyncJsonlWriter& operator=(const AsyncJsonlWriter&) = delete;

  bool start(const AsyncJsonlWriterConfig& config, std::string& err);
  void stop();
  bool enqueue(std::string record);
  AsyncJsonlWriterStatus status() const;

 private:
  void worker_loop();
  bool open_active(std::string& err);
  bool repair_active(std::string& err);
  bool write_record(const std::string& record, std::string& err);
  bool rotate(std::string& err);
  bool gzip_file(const std::string& source, const std::string& target,
                 std::string& err);
  void cleanup_retention();
  void refresh_storage_state();
  void set_error(const std::string& err);

  AsyncJsonlWriterConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::string> queue_;
  std::thread worker_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> storage_available_{false};
  std::atomic<bool> write_enabled_{false};
  std::atomic<bool> low_space_warning_{false};
  std::atomic<int64_t> free_bytes_{0};
  std::atomic<int64_t> queued_bytes_{0};
  std::atomic<int64_t> written_records_{0};
  std::atomic<int64_t> dropped_records_{0};
  std::string active_path_;
  std::string last_error_;
  int fd_ = -1;
  int64_t active_bytes_ = 0;
  int64_t opened_epoch_seconds_ = 0;
  int64_t last_sync_epoch_seconds_ = 0;
};

}  // namespace hzw

#endif  // HZW_PIPELINE_ASYNC_JSONL_WRITER_H
