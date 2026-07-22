// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <folly/init/Init.h>

#include <CLI/CLI.hpp>
#include <iostream>

#include "utils/ConfigCenter.hpp"
#include "utils/Logging.hpp"

using namespace swordfs::utils;

int main(int argc, char* argv[]) {
  // Initialize folly but skip gflags parsing.
  folly::Init folly_init(&argc, &argv, folly::InitOptions().useGFlags(false));

  CLI::App app{"SwordFS - A modern high-performance distributed file system"};

  // Bind and parse CLI options directly to ConfigCenter members
  auto& cfg = swordfs::utils::ConfigCenter::Instance();
  cfg.ConfigureOptions(app);
  cfg.ParseOptions(app, argc, argv);

  // Initialize logging etc
  swordfs::utils::Init();

  // Dispatch the selected subcommand
  auto sub_command = cfg.SelectedSubCommand();
  if (sub_command) {
    return sub_command->run();
  }
  // No subcommand given, show help
  std::cout << app.help();
  return 0;
}
