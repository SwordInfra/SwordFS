// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS logging — configures folly's logging framework and provides
// convenience macros (SWORDFS_LOG_*) that wrap folly's XLOG.
//
// Default: all logs go to /var/log/swordfs.log (INFO level).
// In foreground mode (-f), logs are redirected to stderr.
//
// ── User-facing vs internal logs ───────────────────────────────────
//
//  SWORDFS_LOG_*     → XLOG (metadata-rich: file, line, level).  Goes to
//                       stderr in -f mode, file otherwise.
//
//  SWORDFS_PROMPT     → Plain stderr, no metadata.  For user-visible
//                        errors before daemonizing.  Naturally silent
//                        after Daemonize() (stderr → /dev/null).
//
//  SWORDFS_PROMPT_EXIT → Same, then std::exit(1).

#pragma once

#include <folly/logging/xlog.h>

#include <cstdlib>
#include <iostream>

// ── Internal log macros (XLOG stream style) ────────────────────────────

#define SWORDFS_LOG_INFO  XLOG(INFO)
#define SWORDFS_LOG_DEBUG XLOG(DBG0)
#define SWORDFS_LOG_WARN  XLOG(WARN)
#define SWORDFS_LOG_ERROR XLOG(ERR)
#define SWORDFS_LOG_FATAL XLOG(FATAL)

// ── User-facing terminal output (plain text, no metadata) ──────────────

namespace swordfs::logging::detail {

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

}  // namespace swordfs::logging::detail

#define SWORDFS_PROMPT      swordfs::logging::detail::PromptStream{}
#define SWORDFS_PROMPT_EXIT  swordfs::logging::detail::PromptExitStream{}

namespace swordfs::logging {

/// Initialize folly logging. Reads all settings (foreground mode,
/// log_path, log_level) from ConfigCenter::Instance().
void Init();

}  // namespace swordfs::logging
