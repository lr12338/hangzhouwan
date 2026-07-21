// -*- coding: utf-8 -*-
// DetectionSnapshot 单元测试：有效性、TTL 过期、线程安全更新。
#include <iostream>
#include <thread>
#include "pipeline/detection_snapshot.h"

static int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::cerr << "失败 | " << #cond << " @行" << __LINE__ << "\n";    \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

int main() {
  using hzw::DetectionSnapshot;
  using hzw::SnapshotStore;

  // 默认无效。
  DetectionSnapshot s;
  CHECK(!s.valid());
  CHECK(s.expired(1000, 1000));

  // 有效且未过期。
  s.source_sequence = 5;
  s.source_pts = 42;
  s.generated_time_ms = 1000;
  CHECK(s.valid());
  CHECK(!s.expired(1500, 1000));   // 500ms < 1000ms
  CHECK(!s.expired(2000, 1000));   // 1000ms == ttl，未过期（>才过期）
  CHECK(s.expired(2001, 1000));    // 1001ms > 1000ms，过期

  // SnapshotStore 更新/读取。
  SnapshotStore store;
  store.update(s);
  DetectionSnapshot got = store.get();
  CHECK(got.valid() && got.source_sequence == 5);

  // 覆盖更新。
  DetectionSnapshot s2;
  s2.source_sequence = 9;
  s2.generated_time_ms = 2000;
  store.update(s2);
  got = store.get();
  CHECK(got.source_sequence == 9);

  if (g_failures == 0) std::cout << "通过 | DetectionSnapshot 单元测试全部通过\n";
  return g_failures == 0 ? 0 : 1;
}
