// -*- coding: utf-8 -*-
#include "pipeline/async_jsonl_writer.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <zlib.h>

namespace hzw {
namespace {

int64_t epoch_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string utc_stamp() {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&now, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y%m%dT%H%M%SZ");
  return oss.str();
}

bool ensure_directory(const std::string& path, std::string& err) {
  if (path.empty() || path.front() != '/') {
    err = "事件目录必须为绝对路径";
    return false;
  }
  std::string current;
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i != path.size() && path[i] != '/') continue;
    current = path.substr(0, i);
    if (current.empty()) continue;
    if (::mkdir(current.c_str(), 0750) != 0 && errno != EEXIST) {
      err = "创建目录失败 " + current + ": " + std::strerror(errno);
      return false;
    }
    struct stat st {};
    if (::stat(current.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
      err = "路径不是目录: " + current;
      return false;
    }
  }
  return true;
}

bool ends_with(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool data_is_separate_mount(std::string& err) {
  struct stat root {};
  struct stat data {};
  if (::stat("/", &root) != 0 || ::stat("/data", &data) != 0) {
    err = "检查 /data 挂载失败: " + std::string(std::strerror(errno));
    return false;
  }
  if (!S_ISDIR(data.st_mode) || data.st_dev == root.st_dev) {
    err = "/data 不是独立挂载点，禁止写入根分区";
    return false;
  }
  return true;
}

}  // namespace

AsyncJsonlWriter::~AsyncJsonlWriter() {
  stop();
}

bool AsyncJsonlWriter::start(const AsyncJsonlWriterConfig& config,
                             std::string& err) {
  stop();
  config_ = config;
  if (config_.stream_id.empty() || config_.max_size_mb < 1 ||
      config_.rotate_seconds < 1 || config_.retention_days < 1 ||
      config_.sync_seconds < 1 || config_.queue_max_mb < 1 ||
      config_.disk_stop_mb < 1 ||
      config_.disk_warn_mb <= config_.disk_stop_mb) {
    err = "事件写入器配置非法";
    return false;
  }
  if (config_.directory.compare(0, 6, "/data/") != 0 &&
      config_.directory != "/data") {
    err = "事件目录必须位于 /data，禁止回退根分区";
    return false;
  }
  if (!data_is_separate_mount(err)) return false;
  if (!ensure_directory(config_.directory, err)) return false;

  active_path_ = config_.directory + "/stream_" + config_.stream_id +
                 ".current.jsonl";
  if (!repair_active(err)) return false;
  refresh_storage_state();
  if (!storage_available_.load()) {
    err = "事件目录不可用: " + last_error_;
    return false;
  }
  if (!open_active(err)) return false;

  stop_.store(false);
  running_.store(true);
  worker_ = std::thread(&AsyncJsonlWriter::worker_loop, this);
  return true;
}

void AsyncJsonlWriter::stop() {
  stop_.store(true);
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
  if (fd_ >= 0) {
    ::fdatasync(fd_);
    ::close(fd_);
    fd_ = -1;
  }
  running_.store(false);
}

bool AsyncJsonlWriter::enqueue(std::string record) {
  if (!running_.load()) return false;
  if (record.empty()) return true;
  if (record.back() != '\n') record.push_back('\n');

  const int64_t max_bytes =
      static_cast<int64_t>(config_.queue_max_mb) * 1024 * 1024;
  std::lock_guard<std::mutex> lk(mutex_);
  if (static_cast<int64_t>(record.size()) > max_bytes) {
    dropped_records_.fetch_add(1);
    return false;
  }
  while (!queue_.empty() &&
         queued_bytes_.load() + static_cast<int64_t>(record.size()) > max_bytes) {
    queued_bytes_.fetch_sub(static_cast<int64_t>(queue_.front().size()));
    queue_.pop_front();
    dropped_records_.fetch_add(1);
  }
  queue_.push_back(std::move(record));
  queued_bytes_.fetch_add(static_cast<int64_t>(queue_.back().size()));
  cv_.notify_one();
  return true;
}

AsyncJsonlWriterStatus AsyncJsonlWriter::status() const {
  AsyncJsonlWriterStatus s;
  s.running = running_.load();
  s.storage_available = storage_available_.load();
  s.write_enabled = write_enabled_.load();
  s.low_space_warning = low_space_warning_.load();
  s.free_bytes = free_bytes_.load();
  s.queued_bytes = queued_bytes_.load();
  s.written_records = written_records_.load();
  s.dropped_records = dropped_records_.load();
  s.active_path = active_path_;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    s.last_error = last_error_;
  }
  return s;
}

bool AsyncJsonlWriter::repair_active(std::string& err) {
  int fd = ::open(active_path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0640);
  if (fd < 0) {
    err = "打开活跃事件文件失败: " + std::string(std::strerror(errno));
    return false;
  }
  off_t size = ::lseek(fd, 0, SEEK_END);
  if (size <= 0) {
    ::close(fd);
    return true;
  }
  off_t pos = size;
  bool found_newline = false;
  char buffer[4096];
  while (pos > 0 && !found_newline) {
    const size_t take = static_cast<size_t>(pos > 4096 ? 4096 : pos);
    pos -= static_cast<off_t>(take);
    ssize_t n = ::pread(fd, buffer, take, pos);
    if (n <= 0) break;
    for (ssize_t i = n - 1; i >= 0; --i) {
      if (buffer[i] == '\n') {
        const off_t keep = pos + i + 1;
        if (keep < size && ::ftruncate(fd, keep) != 0) {
          err = "修复事件残行失败: " + std::string(std::strerror(errno));
          ::close(fd);
          return false;
        }
        found_newline = true;
        break;
      }
    }
  }
  if (!found_newline && ::ftruncate(fd, 0) != 0) {
    err = "清理无效事件残行失败: " + std::string(std::strerror(errno));
    ::close(fd);
    return false;
  }
  ::close(fd);
  return true;
}

bool AsyncJsonlWriter::open_active(std::string& err) {
  fd_ = ::open(active_path_.c_str(),
               O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0640);
  if (fd_ < 0) {
    err = "打开事件文件失败: " + std::string(std::strerror(errno));
    return false;
  }
  struct stat st {};
  active_bytes_ = (::fstat(fd_, &st) == 0) ? st.st_size : 0;
  opened_epoch_seconds_ = epoch_seconds();
  last_sync_epoch_seconds_ = opened_epoch_seconds_;
  return true;
}

bool AsyncJsonlWriter::write_record(const std::string& record,
                                    std::string& err) {
  size_t offset = 0;
  while (offset < record.size()) {
    ssize_t n = ::write(fd_, record.data() + offset, record.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      err = "写事件文件失败: " + std::string(std::strerror(errno));
      return false;
    }
    offset += static_cast<size_t>(n);
  }
  active_bytes_ += static_cast<int64_t>(record.size());
  written_records_.fetch_add(1);
  return true;
}

bool AsyncJsonlWriter::gzip_file(const std::string& source,
                                 const std::string& target,
                                 std::string& err) {
  int in = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) {
    err = "打开待压缩事件文件失败: " + std::string(std::strerror(errno));
    return false;
  }
  const std::string temp = target + ".tmp";
  gzFile out = gzopen(temp.c_str(), "wb1");
  if (!out) {
    ::close(in);
    err = "创建 gzip 文件失败";
    return false;
  }
  char buffer[65536];
  bool ok = true;
  for (;;) {
    ssize_t n = ::read(in, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      err = "读取待压缩事件文件失败: " + std::string(std::strerror(errno));
      ok = false;
      break;
    }
    if (n == 0) break;
    if (gzwrite(out, buffer, static_cast<unsigned int>(n)) != n) {
      err = "写 gzip 文件失败";
      ok = false;
      break;
    }
  }
  ::close(in);
  if (gzclose(out) != Z_OK) {
    err = "关闭 gzip 文件失败";
    ok = false;
  }
  if (!ok) {
    ::unlink(temp.c_str());
    return false;
  }
  if (::rename(temp.c_str(), target.c_str()) != 0) {
    err = "发布 gzip 文件失败: " + std::string(std::strerror(errno));
    ::unlink(temp.c_str());
    return false;
  }
  return true;
}

bool AsyncJsonlWriter::rotate(std::string& err) {
  if (fd_ >= 0) {
    ::fdatasync(fd_);
    ::close(fd_);
    fd_ = -1;
  }
  if (active_bytes_ > 0) {
    const std::string archive =
        config_.directory + "/stream_" + config_.stream_id + "-" +
        utc_stamp() + "-" + std::to_string(written_records_.load()) +
        ".jsonl.gz";
    if (!gzip_file(active_path_, archive, err)) {
      // 原始文件保留，下一轮可继续，绝不因压缩失败丢数据。
      return open_active(err) && false;
    }
    if (::unlink(active_path_.c_str()) != 0 && errno != ENOENT) {
      err = "删除已压缩活跃文件失败: " + std::string(std::strerror(errno));
      return false;
    }
  }
  cleanup_retention();
  return open_active(err);
}

void AsyncJsonlWriter::cleanup_retention() {
  DIR* dir = ::opendir(config_.directory.c_str());
  if (!dir) return;
  const std::string prefix = "stream_" + config_.stream_id + "-";
  const int64_t cutoff =
      epoch_seconds() - static_cast<int64_t>(config_.retention_days) * 86400;
  for (dirent* ent = ::readdir(dir); ent != nullptr; ent = ::readdir(dir)) {
    const std::string name = ent->d_name;
    if (name.compare(0, prefix.size(), prefix) != 0 ||
        !ends_with(name, ".jsonl.gz")) {
      continue;
    }
    const std::string path = config_.directory + "/" + name;
    struct stat st {};
    if (::stat(path.c_str(), &st) == 0 && st.st_mtime < cutoff) {
      ::unlink(path.c_str());
    }
  }
  ::closedir(dir);
}

void AsyncJsonlWriter::refresh_storage_state() {
  std::string mount_error;
  if (!data_is_separate_mount(mount_error)) {
    storage_available_.store(false);
    write_enabled_.store(false);
    low_space_warning_.store(true);
    free_bytes_.store(0);
    set_error(mount_error);
    return;
  }
  struct statvfs fs {};
  if (::statvfs(config_.directory.c_str(), &fs) != 0) {
    storage_available_.store(false);
    write_enabled_.store(false);
    low_space_warning_.store(true);
    free_bytes_.store(0);
    set_error("statvfs 失败: " + std::string(std::strerror(errno)));
    return;
  }
  const int64_t free =
      static_cast<int64_t>(fs.f_bavail) * static_cast<int64_t>(fs.f_frsize);
  const int64_t stop_bytes =
      static_cast<int64_t>(config_.disk_stop_mb) * 1024 * 1024;
  const int64_t warn_bytes =
      static_cast<int64_t>(config_.disk_warn_mb) * 1024 * 1024;
  const bool first_success = !storage_available_.load();
  storage_available_.store(true);
  free_bytes_.store(free);
  low_space_warning_.store(free < warn_bytes);
  // 停写后必须恢复到预警水位以上才解除，避免在边界反复启停。
  if (first_success) {
    write_enabled_.store(free >= stop_bytes);
  } else if (write_enabled_.load()) {
    if (free < stop_bytes) write_enabled_.store(false);
  } else if (free >= warn_bytes) {
    write_enabled_.store(true);
  }
}

void AsyncJsonlWriter::set_error(const std::string& err) {
  std::lock_guard<std::mutex> lk(mutex_);
  last_error_ = err;
}

void AsyncJsonlWriter::worker_loop() {
  cleanup_retention();
  while (!stop_.load()) {
    std::string record;
    {
      std::unique_lock<std::mutex> lk(mutex_);
      cv_.wait_for(lk, std::chrono::seconds(1),
                   [this] { return stop_.load() || !queue_.empty(); });
      if (!queue_.empty()) {
        record = std::move(queue_.front());
        queued_bytes_.fetch_sub(static_cast<int64_t>(record.size()));
        queue_.pop_front();
      }
    }

    refresh_storage_state();
    const int64_t now = epoch_seconds();
    if (write_enabled_.load() && !record.empty()) {
      std::string err;
      if (!write_record(record, err)) {
        set_error(err);
        write_enabled_.store(false);
        dropped_records_.fetch_add(1);
      }
    } else if (!record.empty()) {
      dropped_records_.fetch_add(1);
    }

    if (fd_ >= 0 && now - last_sync_epoch_seconds_ >= config_.sync_seconds) {
      if (::fdatasync(fd_) != 0) {
        set_error("fdatasync 失败: " + std::string(std::strerror(errno)));
      }
      last_sync_epoch_seconds_ = now;
    }
    if (fd_ >= 0 && active_bytes_ > 0 &&
        (active_bytes_ >= static_cast<int64_t>(config_.max_size_mb) * 1024 * 1024 ||
         now - opened_epoch_seconds_ >= config_.rotate_seconds)) {
      std::string err;
      if (!rotate(err)) set_error(err);
    }
  }

  // 停止时尽量清空队列；低磁盘保护仍优先于完整落盘。
  for (;;) {
    std::string record;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      if (queue_.empty()) break;
      record = std::move(queue_.front());
      queued_bytes_.fetch_sub(static_cast<int64_t>(record.size()));
      queue_.pop_front();
    }
    refresh_storage_state();
    std::string err;
    if (!write_enabled_.load() || !write_record(record, err)) {
      dropped_records_.fetch_add(1);
      if (!err.empty()) set_error(err);
    }
  }
  if (fd_ >= 0) ::fdatasync(fd_);
  running_.store(false);
}

}  // namespace hzw
