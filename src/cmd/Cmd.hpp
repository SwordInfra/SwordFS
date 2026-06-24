// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS subcommand framework
#pragma once

#include <string_view>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace swordfs::cmd {

/// Arguments passed to a subcommand handler.  argv[0] is the subcommand name.
struct CmdArgs {
  int argc;
  char** argv;
};

/// Describes a single command-line option.
struct OptionDef {
  std::string_view flags;       // e.g. "-f, --foreground"
  std::string_view value;       // e.g. "<path>" or "" if boolean flag
  std::string_view description; // e.g. "Run in foreground"

  /// Print this option as a help entry (e.g. "  -f, --foreground  Run in foreground").
  void Print() const;
};

/// Describes a usage example.
struct Example {
  std::string_view description; // e.g. "# Mount in background"
  std::string_view command;     // e.g. "swordfs mount /mnt/swordfs"
};

/// Each subcommand registers itself with a name, description, options,
/// examples, and handler.
struct Command {
  std::string_view name;
  std::string_view description;
  std::string_view usage;                // e.g. "mount [options] <mountpoint>"
  std::vector<OptionDef> options;
  std::vector<Example> examples;
  std::function<int(const CmdArgs&)> handler;
};

/// Process-wide command registry singleton.
class CommandCenter {
 public:
  /// Returns the singleton instance.
  static CommandCenter& Instance();

  /// Register a subcommand. Called at static-init time.
  void Register(Command cmd);

  /// Dispatch argv to the matching subcommand, or print usage if no match.
  int Dispatch(int argc, char* argv[]);

  /// Print help for a specific subcommand by name.
  /// If the name is not found, prints the top-level usage.
  void ShowHelp(const char* prog, std::string_view cmd_name) const;

 private:
  CommandCenter() = default;
  CommandCenter(const CommandCenter&) = delete;
  CommandCenter& operator=(const CommandCenter&) = delete;

  void PrintUsage(const char* prog) const;
  void PrintCommandHelp(const char* prog, const Command& cmd) const;

 private:
  std::mutex mutex_;
  std::unordered_map<std::string, Command> commands_;
};

}  // namespace swordfs::cmd
