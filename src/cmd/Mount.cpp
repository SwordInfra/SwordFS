// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS mount subcommand

#include <fcntl.h>
#include <folly/portability/Filesystem.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>

#include "cmd/Mount.hpp"
#include "fuse/Vfs.hpp"
#include "utils/ConfigCenter.hpp"
#include "utils/Fuse.hpp"
#include "utils/Logging.hpp"

using namespace swordfs::utils;

namespace swordfs::cmd {

// Mountpoint validation
static int ValidateMountpoint(const std::string& mountpoint) {
  if (mountpoint.empty() || mountpoint[0] == '-') {
    SWORDFS_PROMPT_INFO << "Error: invalid mountpoint '" << mountpoint << "'";
    return -1;
  }
  // Refuse to mount at the root directory — would break the OS
  if (mountpoint == "/") {
    SWORDFS_PROMPT_INFO << "Error: refusing to mount at root directory '/'. "
                           "Mounting at root would render the system unusable.";
    return -1;
  }

  // Detect stale mount
  if (IsStaleMount(mountpoint)) {
    SWORDFS_PROMPT_INFO << "Warning: stale FUSE mount detected at '"
                        << mountpoint
                        << "'. Run 'fusermount3 -u " << mountpoint
                        << "' first.";
    return -1;
  }

  if (!folly::fs::exists(mountpoint)) {
    // Create the mountpoint (and all missing parent directories)
    std::error_code ec;
    folly::fs::create_directories(mountpoint, ec);
    if (ec) {
      SWORDFS_PROMPT_INFO << "Error: failed to create mountpoint '"
                          << mountpoint << "': " << ec.message();
      return -1;
    }
  } else if (!folly::fs::is_directory(mountpoint)) {
    SWORDFS_PROMPT_INFO << "Error: mountpoint '" << mountpoint
                        << "' is not a directory";
    return -1;
  }

  // Must not already be mounted
  if (IsFuseMounted(mountpoint)) {
    SWORDFS_PROMPT_INFO << "Error: mountpoint '" << mountpoint
                        << "' is already mounted";
    return -1;
  }

  return 0;
}

// Daemon mode
//
// Standard double-fork daemonization with pipe-based mount-failure detection.
// The parent blocks on a pipe: receiving a byte means the mount succeeded.
// The intermediate child exits immediately so the grandchild is re-parented
// to PID 1, preventing zombie processes.
//
// Returns the write end of the pipe in the grandchild so MountCmd can signal
// after the mount succeeds; returns -1 on error.
static int Daemonize() {
  int pipefd[2];
  if (::pipe(pipefd) == -1) {
    SWORDFS_PROMPT_FMT("Error: pipe failed: {}", std::strerror(errno));
    return -1;
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    SWORDFS_PROMPT_FMT("Error: daemonization failed: {}", std::strerror(errno));
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    return -1;
  }

  if (pid > 0) {
    // Grandparent: wait for the intermediate child to exit.
    ::close(pipefd[1]);
    char buf;
    ssize_t n = ::read(pipefd[0], &buf, 1);
    ::close(pipefd[0]);
    // Reap the intermediate child to avoid a zombie.
    int status;
    ::waitpid(pid, &status, 0);
    if (n == 1) {
      ::_exit(0);
    }
    SWORDFS_PROMPT_INFO << "Error: daemon failed to mount";
    return -1;
  }

  // Intermediate child: fork again and exit to make grandchild an orphan.
  ::close(pipefd[0]);

  pid = ::fork();
  if (pid < 0) {
    ::_exit(1);  // close the pipe (EOF) to signal failure
  }
  if (pid > 0) {
    // Still the intermediate child — exit immediately so grandchild is
    // adopted by PID 1.
    ::_exit(0);
  }

  // Grandchild (the actual daemon)
  ::setsid();
  ::signal(SIGHUP, SIG_IGN);

  // Redirect stdin/stdout/stderr to /dev/null
  int fd = ::open("/dev/null", O_RDWR);
  if (fd >= 0) {
    ::dup2(fd, STDIN_FILENO);
    ::dup2(fd, STDOUT_FILENO);
    ::dup2(fd, STDERR_FILENO);
    if (fd > 2) ::close(fd);
  }

  return pipefd[1];
}

// Build fuse_args from extra FUSE options only (e.g. -o allow_other).
// The mountpoint is handled separately by fuse_session_mount().
static FuseArgsGuard BuildFuseArgs(const std::vector<std::string>& extras) {
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>("swordfs"));
  for (const auto& e : extras) {
    argv.push_back(const_cast<char*>(e.c_str()));
  }
  return FuseArgsGuard(static_cast<int>(argv.size()), argv.data());
}

// Mount the filesystem via libfuse low-level API. Blocks until unmounted.
static int Mount(const std::string& mountpoint,
                 const std::vector<std::string>& extras, int signal_fd) {
  FuseArgsGuard args(BuildFuseArgs(extras));

  FuseSessionGuard se(fuse_session_new(
      args.get(), &::swordfs::fuse::VfsHookFactory::GetOps(),
      sizeof(struct fuse_lowlevel_ops), nullptr));
  if (!se) {
    return 1;
  }

  if (fuse_set_signal_handlers(se.get()) != 0) {
    return 1;
  }

  if (fuse_session_mount(se.get(), mountpoint.c_str()) != 0) {
    return 1;
  }

  // Signal the parent that the mount succeeded (daemon mode).
  if (signal_fd >= 0) {
    char ok = 0;
    if (::write(signal_fd, &ok, 1) < 0) {
      // Best-effort: parent already exited or pipe broken, nothing to do.
    }
    ::close(signal_fd);
  }

  struct fuse_loop_config* loop_cfg = fuse_loop_cfg_create();
  fuse_loop_cfg_set_max_threads(loop_cfg, ConfigCenter::Instance().fuse_threads());
  int ret = fuse_session_loop_mt(se.get(), loop_cfg);
  fuse_loop_cfg_destroy(loop_cfg);
  return ret;
}

int RunMount() {
  const std::string& mountpoint = ConfigCenter::Instance().mountpoint();
  if (ValidateMountpoint(mountpoint) != 0) {
    return 1;
  }

  // Daemonize by default; -f / --foreground disables this
  int signal_fd = -1;  // pipe fd for child to signal mount success
  if (!ConfigCenter::Instance().foreground()) {
    signal_fd = Daemonize();
    if (signal_fd < 0) {
      return 1;
    }
  }

  auto sub_command = ConfigCenter::Instance().SelectedSubCommand();
  if (!sub_command) {
    return 1;
  }

  // Mount via libfuse low-level API
  //
  // Signal handling is managed by fuse_set_signal_handlers() inside Mount().
  // The low-level session loop will exit cleanly on SIGINT/SIGTERM or when
  // the filesystem is unmounted externally.

  // Collect remaining arguments (e.g. -o allow_other, ro) for FUSE
  std::vector<std::string> fuse_extras = sub_command->cmd->remaining();
  int ret = Mount(mountpoint, fuse_extras, signal_fd);
  if (ret != 0) {
    SWORDFS_PROMPT_FMT("Error: mount failed (code {})", ret);
  }
  return ret;
}

}  // namespace swordfs::cmd
