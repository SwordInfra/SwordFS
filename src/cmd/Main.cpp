// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <folly/init/Init.h>

#include "cmd/Cmd.hpp"
#include "utils/ConfigCenter.hpp"
#include "utils/Logging.hpp"
#include "utils/Status.hpp"


int main(int argc, char* argv[]) {
  // Initialize folly but skip gflags parsing entirely — SwordFS does its
  // own CLI parsing and subcommand dispatch.  Using useGFlags(false) prevents
  // gflags from erroring on flags it doesn't recognize (e.g. -f, mount).
  folly::Init folly_init(&argc, &argv, folly::InitOptions().useGFlags(false));

  // Parse global flags before dispatch.
  swordfs::utils::Status status = swordfs::utils::ConfigCenter::Instance().ParseFromArgs(argc, argv);
  if (!status.ok()) {
    return status.code();
  }

  // Initialize logging
  swordfs::utils::Init();

  return swordfs::cmd::CommandCenter::Instance().Dispatch(argc, argv);
}
