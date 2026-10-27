// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>

namespace swordfs::utils {

// Opt-in #515 diagnostics only. Durations are accumulated client-observed
// operation elapsed times; concurrent operations overlap in real wall time.
// The environment switch is read once, after daemonization in the mount path.
class ExperimentTiming final {
 public:
  enum class Kind { kDataGet, kDataPut, kMetadata, kTransaction };
  struct Metric {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> us{0};
    std::atomic<uint64_t> bytes{0};
  };

  static ExperimentTiming &Instance() {
    static ExperimentTiming instance;
    return instance;
  }

  bool Enabled() const {
    return enabled_;
  }

  class Scope final {
   public:
    explicit Scope(Kind kind, uint64_t bytes = 0) : kind_(kind), bytes_(bytes), enabled_(Instance().Enabled()) {
      if (enabled_) {
        start_ = std::chrono::steady_clock::now();
      }
    }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
    ~Scope() {
      if (!enabled_) {
        return;
      }
      const auto us = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_).count());
      auto &metric = Instance().Select(kind_);
      metric.calls.fetch_add(1, std::memory_order_relaxed);
      metric.us.fetch_add(us, std::memory_order_relaxed);
      metric.bytes.fetch_add(bytes_, std::memory_order_relaxed);
    }

   private:
    Kind kind_;
    uint64_t bytes_;
    bool enabled_;
    std::chrono::steady_clock::time_point start_{};
  };

  void Attempt() {
    if (enabled_) {
      attempts_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  void WatchConflict() {
    if (enabled_) {
      watch_conflicts_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  void Backoff(uint64_t us) {
    if (enabled_) {
      backoff_us_.fetch_add(us, std::memory_order_relaxed);
    }
  }

  std::string SnapshotJson() const {
    const auto metric = [](const Metric &m) {
      std::ostringstream os;
      os << "{\"calls\":" << m.calls.load() << ",\"us\":" << m.us.load() << ",\"bytes\":" << m.bytes.load() << '}';
      return os.str();
    };
    std::ostringstream os;
    os << "{\"data\":{\"get\":" << metric(data_get_) << ",\"put\":" << metric(data_put_)
       << "},\"metadata\":" << metric(metadata_) << ",\"transaction\":{\"calls\":" << transaction_.calls.load()
       << ",\"us\":" << transaction_.us.load() << ",\"attempts\":" << attempts_.load()
       << ",\"watch_conflicts\":" << watch_conflicts_.load() << ",\"backoff_us\":" << backoff_us_.load() << "}}";
    return os.str();
  }

 private:
  ExperimentTiming()
      : enabled_([] {
          const char *flag = std::getenv("SWORDFS_SLICE_EXPERIMENT_TIMING");
          return flag != nullptr && flag[0] == '1' && flag[1] == '\0';
        }()) {
  }
  Metric &Select(Kind kind) {
    switch (kind) {
      case Kind::kDataGet:
        return data_get_;
      case Kind::kDataPut:
        return data_put_;
      case Kind::kMetadata:
        return metadata_;
      case Kind::kTransaction:
        return transaction_;
    }
    return metadata_;
  }
  bool enabled_;
  Metric data_get_;
  Metric data_put_;
  Metric metadata_;
  Metric transaction_;
  std::atomic<uint64_t> attempts_{0};
  std::atomic<uint64_t> watch_conflicts_{0};
  std::atomic<uint64_t> backoff_us_{0};
};

}  // namespace swordfs::utils
