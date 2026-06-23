#include "logging.hpp"

#include <folly/logging/Init.h>
#include <folly/logging/LoggerDB.h>
#include <folly/logging/FileHandlerFactory.h>
#include <folly/logging/StandardLogHandlerFactory.h>
#include <folly/logging/xlog.h>

namespace swordfs::logging {

void Init(bool debug) {
  // Register file handler factory (not registered by default)
  folly::LoggerDB::get().registerHandlerFactory(
      std::make_unique<folly::FileHandlerFactory>());

  if (debug) {
    folly::initLogging(".=DBG0:default; default=stream:stream=stdout");
    XLOG(DBG0, "Logging to stdout (debug mode)");
  } else {
    folly::initLogging(".=INFO:default; default=file:path=/var/log/swordfs.log");
    XLOG(INFO, "Logging to /var/log/swordfs.log");
  }
}

} // namespace swordfs::logging
