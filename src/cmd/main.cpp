// SwordFS entry point
//
// Usage: swordfs [global options] command [command options] [arguments...]

#include "cmd/cmd.hpp"
#include "logging.hpp"

#include <cstring>

#include <folly/init/Init.h>

int main(int argc, char* argv[]) {
  // Initialize folly without stripping flags (we parse --log ourselves)
  folly::Init folly_init(&argc, &argv, /* removeFlags = */ false);

  // Parse --log global option before dispatch
  bool debug_log = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
      debug_log = (std::strcmp(argv[i + 1], "debug") == 0);
      break;
    }
  }

  swordfs::logging::Init(debug_log);

  return swordfs::cmd::Dispatch(argc, argv);
}
