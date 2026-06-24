// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "utils/Logging.hpp"

#include <fcntl.h>
#include <folly/FileUtil.h>
#include <folly/logging/FileHandlerFactory.h>
#include <folly/logging/Init.h>
#include <folly/logging/LogLevel.h>
#include <folly/logging/LoggerDB.h>
#include <folly/logging/StandardLogHandlerFactory.h>
#include <folly/logging/xlog.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include "utils/Config.hpp"

namespace swordfs::utils {

void checkLogLevel(const std::string& level) {
  try {
    folly::stringToLogLevel(level);
  } catch (const std::range_error& e) {
    SWORDFS_PROMPT_EXIT << "Error: invalid log level: " << level;
  }
}

void checkLogFilePath(const std::string& path) {
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

void Init() {
  folly::LoggerDB::get().registerHandlerFactory(
      std::make_unique<folly::FileHandlerFactory>());

  auto& cfg = utils::ConfigCenter::Instance();
  checkLogLevel(cfg.log().level);

  if (cfg.foreground()) {
    folly::initLogging(".=INFO:default; default=stream:stream=stderr");
  } else {
    checkLogFilePath(cfg.log().path);
    std::string config = ".=" + cfg.log().level +
                         ":default; default=file:path=" + cfg.log().path;
    folly::initLogging(config.c_str());
    SWORDFS_LOG_INFO << "Logging to " << cfg.log().path << " at level "
                     << cfg.log().level;
  }
}

}  // namespace swordfs::utils
