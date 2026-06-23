// SwordFS subcommand framework
//
// Modeled after JuiceFS's urfave/cli pattern:
//   - Each subcommand is a function matching the CommandFunc signature
//   - The dispatcher routes argv[1] to the appropriate handler

#pragma once

#include <string_view>
#include <functional>
#include <vector>

namespace swordfs::cmd {

/// Arguments passed to a subcommand handler.  argv[0] is the subcommand name.
struct CmdArgs {
  int argc;
  char** argv;
};

/// Each subcommand registers itself with a name, description, and handler.
struct Command {
  std::string_view name;
  std::string_view description;
  std::function<int(const CmdArgs&)> handler;
};

/// Register a subcommand. Called at static-init time.
void RegisterCommand(Command cmd);

/// Dispatch argv[1] to the matching subcommand, or print usage if no match.
int Dispatch(int argc, char* argv[]);

} // namespace swordfs::cmd
