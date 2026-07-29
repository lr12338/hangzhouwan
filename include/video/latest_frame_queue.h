// -*- coding: utf-8 -*-
// =============================================================================
// 最新帧队列（LatestFrameQueue）：固定容量、主动丢旧帧的低延迟队列。
//
// 策略：
//   - 容量固定（默认 1）；push 时若已满，丢弃最旧的一帧（调用 releaser 释放资源），
//     再入队新帧，保证队列长度 <= capacity，延迟不随时间增长。
//   - close() 唤醒所有阻塞消费者；pop 在队空且已关闭时返回 false，消费者据此退出。
//   - 线程安全。
//
// 设计为模板，便于单元测试用简单结构验证“丢旧/关闭/容量”逻辑，不依赖 FFmpeg。
// =============================================================================
#ifndef HZW_VIDEO_LATEST_FRAME_QUEUE_H
#define HZW_VIDEO_LATEST_FRAME_QUEUE_H

#include <cstddef>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

namespace hzw {

template <typename T>
class LatestFrameQueue {
 public:
  using Releaser = std::function<void(T&)>;

  explicit LatestFrameQueue(std::size_t capacity = 1, Releaser releaser = [](T&) {})
      : capacity_(capacity == 0 ? 1 : capacity), releaser_(std::move(releaser)) {}

  // 入队：满则丢最旧帧。item 被移动入队。
  void push(T item) {
    std::lock_guard<std::mutex> lk(m_);
    while (q_.size() >= capacity_) {
      T old = std::move(q_.front());
      q_.pop_front();
      releaser_(old);
      ++dropped_;
    }
    q_.push_back(std::move(item));
    cv_.notify_one();
  }

  // 阻塞出队；队空且已关闭时返回 false（消费者退出条件）。
  bool pop(T& out) {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [this] { return !q_.empty() || closed_; });
    if (q_.empty()) return false;  // 已关闭且空
    out = std::move(q_.front());
    q_.pop_front();
    return true;
  }

  // 非阻塞尝试出队；无帧返回 false。
  bool try_pop(T& out) {
    std::lock_guard<std::mutex> lk(m_);
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    return true;
  }

  void close() {
    std::lock_guard<std::mutex> lk(m_);
    closed_ = true;
    cv_.notify_all();
  }

  bool closed() const {
    std::lock_guard<std::mutex> lk(m_);
    return closed_;
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lk(m_);
    return q_.size();
  }

  std::size_t capacity() const { return capacity_; }
  std::size_t dropped() const {
    std::lock_guard<std::mutex> lk(m_);
    return dropped_;
  }

  // 释放队列内残留帧（析构前调用，避免资源泄漏）。
  void drain() {
    std::lock_guard<std::mutex> lk(m_);
    while (!q_.empty()) {
      T old = std::move(q_.front());
      q_.pop_front();
      releaser_(old);
    }
  }

 private:
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::deque<T> q_;
  std::size_t capacity_;
  bool closed_ = false;
  std::size_t dropped_ = 0;
  Releaser releaser_;
};

}  // namespace hzw

#endif  // HZW_VIDEO_LATEST_FRAME_QUEUE_H
