#include "cmd/cmd.hpp"
#include <swordfs_version.h>

#include <cstring>
#include <iostream>
#include <mutex>
#include <vector>

namespace swordfs::cmd {

static std::mutex registry_mutex;
static std::vector<Command>* command_registry = nullptr;

void RegisterCommand(Command cmd) {
  std::scoped_lock lock(registry_mutex);
  if (!command_registry) {
    command_registry = new std::vector<Command>();
  }
  command_registry->push_back(std::move(cmd));
}

static void PrintUsage(const char* prog) {
  std::cout << "usage: " << prog
            << " [global options] command [command options] [arguments...]\n\n"
            << "SwordFS — A modern high-performance distributed file system "
            << "for AI/ML workloads\n\n"
            << "Commands:\n";

  if (command_registry) {
    for (const auto& cmd : *command_registry) {
      std::cout << "  " << cmd.name;
      auto pad = 12 - cmd.name.size();
      if (pad < 2) pad = 2;
      std::cout << std::string(pad, ' ') << cmd.description << "\n";
    }
  }

  std::cout << "\n"
            << "Global options:\n"
            << "  -h, --help     Show this help message\n"
            << "  -V, --version  Show version information\n"
            << "  --log <level>  Log level: file (default, to /var/log/swordfs.log)\n"
            << "                 or debug (to stdout)\n"
            << "\n"
            << "Run '" << prog << " command --help' for more information on a command.\n";
}

int Dispatch(int argc, char* argv[]) {
  if (argc < 2) {
    PrintUsage(argv[0]);
    return 1;
  }

  const char* subcmd = argv[1];

  if (std::strcmp(subcmd, "-h") == 0 || std::strcmp(subcmd, "--help") == 0) {
    PrintUsage(argv[0]);
    return 0;
  }

  if (std::strcmp(subcmd, "-V") == 0 || std::strcmp(subcmd, "--version") == 0) {
    std::cout << "SwordFS version " << SWORDFS_VERSION_MAJOR << "."
              << SWORDFS_VERSION_MINOR << "." << SWORDFS_VERSION_PATCH << "\n";
    return 0;
  }

  if (!command_registry) {
    std::cerr << "Error: no subcommands registered\n";
    return 1;
  }

  for (const auto& cmd : *command_registry) {
    if (cmd.name == subcmd) {
      // Pass argv[1..] as the subcommand's argument vector with argv[0] = subcmd
      CmdArgs cmdArgs{ argc - 1, argv + 1 };
      return cmd.handler(cmdArgs);
    }
  }

  std::cerr << "Error: unknown command '" << subcmd << "'\n";
  PrintUsage(argv[0]);
  return 1;
}

} // namespace swordfs::cmd
