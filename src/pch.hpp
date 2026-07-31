// ────────────────────────────────────────────────────────────────
// Precompiled header for SwordFS
//
// All heavy, frequently-used third-party and STL headers are
// gathered here so that clang/clangd compile them once and reuse
// the serialized AST across all 46 translation units.
//
// DO NOT include project headers here — they change frequently
// and would invalidate the PCH for every edit.
// ────────────────────────────────────────────────────────────────

// Standard library (C++20) — ordered for stable compilation
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// POSIX / system
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// fmt (lightweight, used by folly logging)
#include <fmt/core.h>

// ────────────────────────────────────────────────────────────────
// folly — the heaviest dependency (~1019 headers, template-heavy)
// ────────────────────────────────────────────────────────────────
#include <folly/FileUtil.h>
#include <folly/container/F14Map.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManager.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/futures/Future.h>
#include <folly/init/Init.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBase.h>
#include <folly/json.h>
#include <folly/logging/FileHandlerFactory.h>
#include <folly/logging/Init.h>
#include <folly/logging/LogLevel.h>
#include <folly/logging/LoggerDB.h>
#include <folly/logging/xlog.h>
#include <folly/portability/Filesystem.h>
