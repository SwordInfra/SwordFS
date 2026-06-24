// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS logging — configures folly's logging framework and provides
// convenience macros (SWORDFS_LOG_*) that wrap folly's XLOG.

#pragma once

#include <fmt/core.h>
#include <folly/logging/xlog.h>

#include <cstdlib>
#include <iostream>

// Internal log macros (XLOG stream style)

#define SWORDFS_LOG_INFO XLOG(INFO)
#define SWORDFS_LOG_DEBUG XLOG(DBG0)
#define SWORDFS_LOG_WARN XLOG(WARN)
#define SWORDFS_LOG_ERROR XLOG(ERR)
#define SWORDFS_LOG_FATAL XLOG(FATAL)

// User-facing terminal output (plain text, no metadata)

struct PromptStream {
  ~PromptStream() { std::cerr << std::endl; }
  template <typename T>
  PromptStream& operator<<(T&& val) {
    std::cerr << std::forward<T>(val);
    return *this;
  }
};

struct PromptExitStream {
  ~PromptExitStream() {
    std::cerr << std::endl;
    std::exit(1);
  }
  template <typename T>
  PromptExitStream& operator<<(T&& val) {
    std::cerr << std::forward<T>(val);
    return *this;
  }
};

template <typename... Args>
void PromptFmt(fmt::format_string<Args...> fmt_str, Args&&... args) {
  std::cerr << fmt::format(fmt_str, std::forward<Args>(args)...) << std::endl;
}

// clang-format off
#define SWORDFS_PROMPT_INFO ::PromptStream {}
#define SWORDFS_PROMPT_FMT ::PromptFmt
#define SWORDFS_PROMPT_EXIT ::PromptExitStream {}
// clang-format on

namespace swordfs::utils {

/// Initialize folly logging. Reads all settings (foreground mode,
/// log_path, log_level) from ConfigCenter::Instance().
void Init();

}  // namespace swordfs::utils
