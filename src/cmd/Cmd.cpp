// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "cmd/Cmd.hpp"
#include <swordfs_version.h>

#include <cstring>
#include <iostream>
#include <mutex>

namespace swordfs::cmd {

// Global options (applied before the subcommand name) 
static const std::vector<OptionDef> kGlobalOptions = {
  {"-h, --help",        "",    "Show this help message"},
  {"-V, --version",     "",    "Show version information"},
  {"-f, --foreground",  "",    "Run in foreground (default: daemonize)"},
  {"--log-file",        "<path>", "Log file path (default: /var/log/swordfs.log)"},
  {"--log-level",       "<lvl>",  "Log level: INFO, DBG0, WARN, ERR (default: INFO)"},
};

void OptionDef::Print() const {
  std::cout << "  " << flags;
  if (!value.empty()) {
    std::cout << " " << value;
  }
  auto pad = 24 - flags.size() - (value.empty() ? 0 : value.size() + 1);
  if (pad < 2) pad = 2;
  std::cout << std::string(pad, ' ') << description << "\n";
}

CommandCenter& CommandCenter::Instance() {
  static CommandCenter instance;
  return instance;
}

void CommandCenter::Register(Command cmd) {
  std::lock_guard<std::mutex> lock(mutex_);
  commands_.emplace(std::string(cmd.name), std::move(cmd));
}

void CommandCenter::PrintUsage(const char* prog) const {
  // Build the global-options portion of the usage line from kGlobalOptions.
  // e.g. "swordfs [-h] [-V] [-f] [--log-file <path>] [--log-level <lvl>]"
  std::cout << "Usage: " << prog;
  for (const auto& opt : kGlobalOptions) {
    std::cout << " [" << opt.flags;
    if (!opt.value.empty()) {
      std::cout << " " << opt.value;
    }
    std::cout << "]";
  }
  std::cout << " command [command options] [arguments...]\n\n"
            << "SwordFS — A modern high-performance distributed file system\n\n"
            << "Commands:\n";

  for (const auto& [name, cmd] : commands_) {
    std::cout << "  " << name;
    auto pad = 12 - name.size();
    if (pad < 2) pad = 2;
    std::cout << std::string(pad, ' ') << cmd.description << "\n";
  }

  std::cout << "\n"
            << "Global options:\n";
  for (const auto& opt : kGlobalOptions) {
    opt.Print();
  }

  std::cout << "\nRun '" << prog << " command --help' for more information on a command.\n";
}

void CommandCenter::PrintCommandHelp(const char* prog,
                                     const Command& cmd) const {
  std::cout << "Usage: " << prog << " " << cmd.usage << "\n\n"
            << cmd.description << "\n";

  if (!cmd.options.empty()) {
    std::cout << "Options:\n";
    for (const auto& opt : cmd.options) {
      opt.Print();
    }
  }

  if (!cmd.examples.empty()) {
    std::cout << "\nExamples:\n";
    for (const auto& ex : cmd.examples) {
      std::cout << ex.description << "\n"
                << "  " << ex.command << "\n\n";
    }
  }
}

void CommandCenter::ShowHelp(const char* prog,
                             std::string_view cmd_name) const {
  auto it = commands_.find(std::string(cmd_name));
  if (it != commands_.end()) {
    PrintCommandHelp(prog, it->second);
    return;
  }
  PrintUsage(prog);
}

int CommandCenter::Dispatch(int argc, char* argv[]) {
  // Skip past known global flags to find the subcommand name.
  int cmd_index = 1;
  while (cmd_index < argc) {
    const char* arg = argv[cmd_index];
    if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
      PrintUsage(argv[0]);
      return 0;
    }
    if (std::strcmp(arg, "-V") == 0 || std::strcmp(arg, "--version") == 0) {
      std::cout << "SwordFS version " << SWORDFS_VERSION_MAJOR << "."
                << SWORDFS_VERSION_MINOR << "." << SWORDFS_VERSION_PATCH << "\n";
      return 0;
    }
    if (std::strcmp(arg, "-f") == 0 || std::strcmp(arg, "--foreground") == 0) {
      ++cmd_index;
      continue;
    }
    if ((std::strcmp(arg, "--log-file") == 0 ||
         std::strcmp(arg, "--log-level") == 0) &&
        cmd_index + 1 < argc) {
      cmd_index += 2;
      continue;
    }
    break;
  }

  if (cmd_index >= argc) {
    PrintUsage(argv[0]);
    return 1;
  }

  const char* subcmd = argv[cmd_index];

  auto it = commands_.find(std::string(subcmd));
  if (it != commands_.end()) {
    CmdArgs cmdArgs{ argc - cmd_index, argv + cmd_index };
    return it->second.handler(cmdArgs);
  }

  std::cerr << "Error: unknown command '" << subcmd << "'\n";
  PrintUsage(argv[0]);
  return 1;
}

} // namespace swordfs::cmd
