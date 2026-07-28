// -*- coding: utf-8 -*-
#include "pipeline/async_jsonl_writer.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace hzw;

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { \
  std::cerr << "FAIL " << #x << " line=" << __LINE__ << "\n"; ++failures; \
} } while (0)

std::vector<std::string> files(const std::string& directory) {
  std::vector<std::string> out;
  DIR* d = ::opendir(directory.c_str());
  if (!d) return out;
  for (dirent* e = ::readdir(d); e; e = ::readdir(d)) {
    if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, ".."))
      out.emplace_back(e->d_name);
  }
  ::closedir(d);
  return out;
}

std::string gunzip(const std::string& path) {
  std::string out;
  gzFile f = gzopen(path.c_str(), "rb");
  if (!f) return out;
  char buffer[4096];
  int n = 0;
  while ((n = gzread(f, buffer, sizeof(buffer))) > 0) out.append(buffer, n);
  gzclose(f);
  return out;
}

void cleanup(const std::string& directory) {
  for (const auto& name : files(directory))
    ::unlink((directory + "/" + name).c_str());
  ::rmdir(directory.c_str());
}
}  // namespace

int main() {
  const std::string directory =
      "/data/hangzhouwan/events/test-async-jsonl-" +
      std::to_string(::getpid());
  CHECK(::mkdir(directory.c_str(), 0750) == 0);
  const std::string active = directory + "/stream_A.current.jsonl";
  {
    std::ofstream f(active);
    f << "{\"old\":1}\n{\"partial\":";
  }

  AsyncJsonlWriterConfig cfg;
  cfg.directory = directory;
  cfg.stream_id = "A";
  cfg.max_size_mb = 1;
  cfg.rotate_seconds = 2;
  cfg.retention_days = 7;
  cfg.sync_seconds = 1;
  cfg.queue_max_mb = 1;
  cfg.disk_warn_mb = 2;
  cfg.disk_stop_mb = 1;

  AsyncJsonlWriter writer;
  std::string err;
  CHECK(writer.start(cfg, err));
  CHECK(writer.enqueue("{\"schema_version\":1,\"n\":1}"));
  CHECK(writer.enqueue("{\"schema_version\":1,\"n\":2}\n"));
  std::this_thread::sleep_for(std::chrono::seconds(4));
  writer.stop();
  auto status = writer.status();
  CHECK(status.written_records == 2);
  CHECK(status.dropped_records == 0);

  std::string archive;
  for (const auto& name : files(directory)) {
    if (name.size() > 9 &&
        name.substr(name.size() - 9) == ".jsonl.gz") {
      archive = directory + "/" + name;
    }
  }
  CHECK(!archive.empty());
  const std::string content = gunzip(archive);
  CHECK(content.find("{\"old\":1}\n") != std::string::npos);
  CHECK(content.find("\"n\":1") != std::string::npos);
  CHECK(content.find("\"partial\"") == std::string::npos);

  const std::string low_directory = directory + "-low";
  CHECK(::mkdir(low_directory.c_str(), 0750) == 0);
  cfg.directory = low_directory;
  cfg.disk_stop_mb = 99998;
  cfg.disk_warn_mb = 99999;
  AsyncJsonlWriter protected_writer;
  CHECK(protected_writer.start(cfg, err));
  CHECK(protected_writer.enqueue("{\"schema_version\":1,\"n\":3}"));
  std::this_thread::sleep_for(std::chrono::seconds(2));
  protected_writer.stop();
  status = protected_writer.status();
  CHECK(!status.write_enabled);
  CHECK(status.dropped_records >= 1);

  cleanup(low_directory);
  cleanup(directory);
  if (failures == 0) std::cout << "通过 | 异步 JSONL 写入器测试\n";
  return failures == 0 ? 0 : 1;
}
