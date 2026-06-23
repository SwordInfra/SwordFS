// SwordFS logging — configures folly's logging framework.
//
// Default: all logs go to /var/log/swordfs.log (INFO level).
// --log debug: all logs go to stdout (DBG0 level).
//
// Mount pre-flight validation errors always go to stderr, regardless of
// log settings, so they are visible to the user at the terminal.

#pragma once

#include <string_view>

namespace swordfs::logging {

/// Initialize folly logging.
///
/// @param debug  if true, log to stdout at DBG0; otherwise to file at INFO.
void Init(bool debug);

} // namespace swordfs::logging
