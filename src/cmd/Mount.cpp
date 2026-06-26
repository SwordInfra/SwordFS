// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS mount subcommand

#include <fcntl.h>
#include <folly/portability/Filesystem.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <system_error>

#define FUSE_USE_VERSION 312
#include <SwordfsVersion.h>
#include <fuse_lowlevel.h>
#include <fuse_opt.h>

#include "cmd/Cmd.hpp"
#include "fuse/Vfs.hpp"
#include "utils/ConfigCenter.hpp"
#include "utils/Fuse.hpp"
#include "utils/Logging.hpp"

using namespace swordfs::utils;

namespace swordfs::cmd::mount {

// Mount-specific option flags
#define MOUNT_OPT(t, p, v) {t, offsetof(struct MountFlags, p), v}

enum {
  kHelp = 0,
  kForeground = 1,
  kVersion = 2,
};

struct MountFlags {
  bool action_processed{
      false};  // true if MountOptProc handled --help or --version
  int foreground{
      0};  // -f, --foreground: stay in foreground (default: daemonize)
  char* mountpoint{};
};

static struct fuse_opt mount_opts[] = {
    FUSE_OPT_KEY("-h", kHelp),
    FUSE_OPT_KEY("--help", kHelp),
    MOUNT_OPT("-f", foreground, kForeground),
    MOUNT_OPT("--foreground", foreground, kForeground),
    FUSE_OPT_KEY("-V", kVersion),
    FUSE_OPT_KEY("--version", kVersion),
    FUSE_OPT_END};

static int ValidateMountpoint(const std::string& mountpoint) {
  if (mountpoint[0] == '-') {
    SWORDFS_PROMPT_INFO << "Error: mountpoint must not start with '-'";
    return -1;
  }
  // Refuse to mount at the root directory — would break the OS
  if (mountpoint == "/") {
    SWORDFS_PROMPT_INFO << "Error: refusing to mount at root directory '/'. "
                           "Mounting at root would render the system unusable.";
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

  // Detect stale mount
  if (IsStaleMount(mountpoint)) {
    SWORDFS_PROMPT_INFO << "Warning: stale FUSE mount detected at '"
                        << mountpoint
                        << "'. You may need to run 'fusermount3 -u "
                        << mountpoint << "' first.";
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

// --help and --version are FUSE_OPT_KEY entries: they don't map to a
// MountFlags field but instead trigger an immediate action. MountOptProc
// handles them and sets flags->action_processed so MountCmd can exit cleanly.
static int MountOptProc(void* data, const char* /*arg*/, int key,
                        struct fuse_args* /*outargs*/) {
  auto* flags = static_cast<MountFlags*>(data);
  if (key == kHelp) {
    ::swordfs::cmd::CommandCenter::Instance().ShowHelp("mount");
    flags->action_processed = true;
    return 0;
  }
  if (key == kVersion) {
    SWORDFS_PROMPT_FMT("SwordFS version {}.{}.{} (libfuse {}.{})",
                       SWORDFS_VERSION_MAJOR, SWORDFS_VERSION_MINOR,
                       SWORDFS_VERSION_PATCH, FUSE_MAJOR_VERSION,
                       FUSE_MINOR_VERSION);
    flags->action_processed = true;
    return 0;
  }
  return 1;
}

// Mount the filesystem via libfuse low-level API. Blocks until unmounted.
int Mount(int argc, char* argv[], int signal_fd) {
  FuseArgsGuard args(argc, argv);

  FuseSessionGuard se(fuse_session_new(
      args.get(), &::swordfs::fuse::VfsHookFactory::GetOps(),
      sizeof(struct fuse_lowlevel_ops), nullptr));
  if (!se) {
    return 1;
  }

  char* mountpoint = nullptr;
  if (args->argc != 1 || args->argv[0][0] == '-') {
    SWORDFS_PROMPT_INFO << "Error: no mountpoint specified";
    return 1;
  } else {
    mountpoint = args->argv[0];
  }

  if (fuse_set_signal_handlers(se.get()) != 0) {
    return 1;
  }

  if (fuse_session_mount(se.get(), mountpoint) != 0) {
    return 1;
  }

  // Signal the parent that the mount succeeded (daemon mode).
  if (signal_fd >= 0) {
    char ok = 0;
    ::write(signal_fd, &ok, 1);
    ::close(signal_fd);
  }

  struct fuse_loop_config* loop_cfg = fuse_loop_cfg_create();
  fuse_loop_cfg_set_max_threads(loop_cfg, ConfigCenter::Instance().fuse_threads());
  int ret = fuse_session_loop_mt(se.get(), loop_cfg);
  fuse_loop_cfg_destroy(loop_cfg);
  return ret;
}

int MountCmd(const ::swordfs::cmd::CmdArgs& args) {
  // Parse mount-specific options
  MountFlags flags{};
  FuseArgsGuard fargs(args.argc, args.argv);

  int ret = fuse_opt_parse(fargs.get(), &flags, mount_opts, MountOptProc);
  if (ret == -1) {
    SWORDFS_PROMPT_INFO << "Error: mount options parsing failed";
    ::swordfs::cmd::CommandCenter::Instance().ShowHelp("mount");
    return 1;
  }
  if (flags.action_processed) {
    // MountOptProc already handled --help or --version
    return 0;
  }

  // Extract mountpoint (last positional argument)
  std::string mountpoint = fargs->argv[fargs->argc - 1];
  if (ValidateMountpoint(mountpoint) != 0) {
    return 1;
  }

  // Daemonize
  // Default: fork to background (like libfuse fuse_main).
  // -f / --foreground is parsed globally and stored in Config;
  // mount_opts still consumes "-f" so it doesn't leak to libfuse.
  int signal_fd = -1;  // pipe fd for child to signal mount success
  if (!::swordfs::utils::ConfigCenter::Instance().foreground()) {
    signal_fd = Daemonize();
    if (signal_fd < 0) {
      return 1;
    }
  }

  // Mount via libfuse low-level API
  //
  // Signal handling is managed by fuse_set_signal_handlers() inside
  // swordfs::fuse::Mount(). The low-level session loop will exit cleanly
  // on SIGINT/SIGTERM or when the filesystem is unmounted externally.
  //
  // Skip argv[0] (the subcommand name "mount") — only pass
  // remaining options + mountpoint to libfuse for layered parsing.
  ret = Mount(fargs->argc - 1, fargs->argv + 1, signal_fd);

  if (ret != 0) {
    SWORDFS_PROMPT_FMT("Error: mount failed (code {})", ret);
  }

  return ret;
}

}  // namespace swordfs::cmd::mount

// Auto-registration
namespace {
struct AutoRegister {
  AutoRegister() {
    swordfs::cmd::CommandCenter::Instance().Register({
        .name = "mount",
        .description = "Mount a filesystem with SwordFS",
        .usage = "mount [options] <mountpoint>",
        .options =
            {
                {"-f, --foreground", "",
                 "Run in foreground (default: daemonize)"},
                {"-o,", "opt[,opt...]",
                 "FUSE mount options (see mount.fuse3(8))"},
                {"-h, --help", "", "Show this help message"},
                {"-V, --version", "", "Show version information"},
            },
        .examples =
            {
                {"# Mount in background (default)",
                 "swordfs mount /mnt/swordfs"},
                {"# Mount in foreground", "swordfs mount -f /mnt/swordfs"},
                {"# Mount with FUSE options",
                 "swordfs mount -o allow_other,ro /mnt/swordfs"},
            },
        .handler = swordfs::cmd::mount::MountCmd,
    });
  }
};
static AutoRegister _;
}  // anonymous namespace
