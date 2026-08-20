// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "utils/Logging.hpp"

#include <fcntl.h>
#include <folly/FileUtil.h>
#include <folly/logging/FileHandlerFactory.h>
#include <folly/logging/Init.h>
#include <folly/logging/LogLevel.h>
#include <folly/logging/LoggerDB.h>
#include <folly/logging/xlog.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace swordfs::utils {

void checkLogLevel(const std::string &level) {
  try {
    folly::stringToLogLevel(level);
  } catch (const std::range_error &e) {
    SWORDFS_PROMPT_EXIT << "Error: invalid log level: " << level;
  }
}

void checkLogFilePath(const std::string &path) {
  if (path.empty()) {
    SWORDFS_PROMPT_EXIT << "Error: log file path is empty";
  }
  // Verify the log file is accessible (create if missing).
  int fd = folly::openNoInt(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    SWORDFS_PROMPT_EXIT << "Error: cannot open log file '" << path
                        << "': " << std::strerror(errno);
  }
  ::close(fd);
}

void InitLogging(const LogConfig &log, bool foreground) {
  folly::LoggerDB::get().registerHandlerFactory(
      std::make_unique<folly::FileHandlerFactory>());

  checkLogLevel(log.level);

  if (foreground) {
    folly::initLogging(".=INFO:default; default=stream:stream=stderr");
  } else {
    checkLogFilePath(log.path);
    std::string config = ".=" + log.level +
                         ":default; default=file:path=" + log.path;
    folly::initLogging(config.c_str());
    SWORDFS_LOG_INFO << "Logging to " << log.path << " at level " << log.level;
  }
}

}  // namespace swordfs::utils
