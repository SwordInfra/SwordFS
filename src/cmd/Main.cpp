// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <folly/init/Init.h>

#include <sys/resource.h>

#include <CLI/CLI.hpp>
#include <cstdlib>
#include <iostream>

#include "config/ConfigCenter.hpp"
#include "utils/Logging.hpp"

using namespace swordfs::utils;

// ────────────────────────────────────────────────────────────────
// Process-level setup driven by environment variables.
// Add new env-var gated behaviors here.
// ────────────────────────────────────────────────────────────────

static void SetupProcessFromEnv() {
  // SWORDFS_ENABLE_COREDUMP — allow core dumps for crash debugging.
  if (std::getenv("SWORDFS_ENABLE_COREDUMP")) {
    struct rlimit rl = {RLIM_INFINITY, RLIM_INFINITY};
    setrlimit(RLIMIT_CORE, &rl);
  }
}

int main(int argc, char* argv[]) {
  SetupProcessFromEnv();

  // Initialize folly but skip gflags parsing.
  folly::Init folly_init(&argc, &argv, folly::InitOptions().useGFlags(false));

  CLI::App app{"SwordFS - A modern high-performance distributed file system"};
  app.allow_extras(false);

  // Bind CLI options to ConfigCenter members
  auto& cfg = swordfs::config::ConfigCenter::Instance();
  cfg.ConfigureOptions(app);

  // Parse CLI arguments
  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }

  // Dispatch the selected subcommand
  auto sub_command = cfg.SelectedSubCommand();
  if (!sub_command) {
    // No subcommand given, show help
    std::cout << app.help();
    return 0;
  }
  // Initialize logging only when executing a subcommand
  swordfs::utils::Init();
  return sub_command->run();
}
