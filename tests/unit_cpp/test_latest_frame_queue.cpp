// -*- coding: utf-8 -*-
// 最新帧队列单元测试：容量、丢旧帧、关闭后消费者退出、丢弃计数。
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>
#include "video/latest_frame_queue.h"

static int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::cerr << "失败 | " << #cond << " @行" << __LINE__ << "\n";    \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

struct Frame { int seq = 0; };

int main() {
  using Q = hzw::LatestFrameQueue<Frame>;

  // 1) 容量1丢旧：push 1,2 后 pop 应得到 2（最新）。
  {
    Q q(1);
    q.push(Frame{1});
    q.push(Frame{2});
    CHECK(q.size() == 1);
    Frame f{};
    CHECK(q.pop(f) && f.seq == 2);
    CHECK(q.dropped() == 1);
  }
  // 2) 容量2保留最新2帧。
  {
    Q q(2);
    q.push(Frame{1}); q.push(Frame{2}); q.push(Frame{3});
    CHECK(q.size() == 2);
    Frame f{};
    CHECK(q.pop(f) && f.seq == 2);  // 1 被丢弃
    CHECK(q.pop(f) && f.seq == 3);
    CHECK(q.dropped() == 1);
  }
  // 3) 关闭后消费者退出：消费者线程 pop 阻塞，close 后返回 false。
  {
    Q q(1);
    std::atomic<bool> exited(false);
    std::thread consumer([&] {
      Frame f{};
      while (q.pop(f)) { /* 消费 */ }
      exited = true;
    });
    q.push(Frame{10});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!exited.load());  // 仍阻塞等待
    q.close();
    consumer.join();
    CHECK(exited.load());
  }
  // 4) 先关闭再 pop：空队列立即返回 false。
  {
    Q q(1);
    q.close();
    Frame f{};
    CHECK(!q.pop(f));
  }
  // 5) releaser 被调用（丢帧时）。
  {
    int released = 0;
    Q q(1, [&released](Frame&) { ++released; });
    q.push(Frame{1}); q.push(Frame{2});  // 1 被丢弃并 release
    CHECK(released == 1);
    Frame f{}; q.pop(f);
    CHECK(released == 1);  // 出队不 release
    q.drain();
  }

  if (g_failures == 0) std::cout << "通过 | 最新帧队列单元测试全部通过\n";
  return g_failures == 0 ? 0 : 1;
}
