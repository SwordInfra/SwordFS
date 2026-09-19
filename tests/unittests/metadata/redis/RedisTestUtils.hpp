// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace swordfs::test {

inline std::string UniqueRedisTestNamespace(std::string_view prefix, std::string_view suffix = {}) {
  static const uint64_t process_run_token = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
  static std::atomic<uint64_t> sequence{0};

  std::string name;
  name.reserve(prefix.size() + suffix.size() + 64);
  name.append(prefix);
  name.push_back('-');
  name.append(std::to_string(::getpid()));
  name.push_back('-');
  name.append(std::to_string(process_run_token));
  name.push_back('-');
  name.append(std::to_string(sequence.fetch_add(1, std::memory_order_relaxed) + 1));
  if (!suffix.empty()) {
    name.push_back('-');
    name.append(suffix);
  }
  return name;
}

}  // namespace swordfs::test
