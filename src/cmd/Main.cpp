// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "cmd/Cmd.hpp"
#include "utils/Logging.hpp"
#include "utils/Config.hpp"

#include <folly/init/Init.h>

int main(int argc, char* argv[]) {
  // Initialize folly but skip gflags parsing entirely — SwordFS does its
  // own CLI parsing and subcommand dispatch.  Using useGFlags(false) prevents
  // gflags from erroring on flags it doesn't recognize (e.g. -f, mount).
  folly::Init folly_init(&argc, &argv, folly::InitOptions().useGFlags(false));

  // Parse global flags before dispatch.
  swordfs::config::ConfigCenter::Instance().ParseFromArgs(argc, argv);

  // Initialize logging
  swordfs::logging::Init();

  return swordfs::cmd::CommandCenter::Instance().Dispatch(argc, argv);
}
