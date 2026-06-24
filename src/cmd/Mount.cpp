// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS mount subcommand
//
// Usage: swordfs mount [options] <mountpoint>
//
// Modeled after JuiceFS cmd/mount.go and cmd/mount_unix.go.
// Orchestrates: argument parsing → mountpoint validation → daemon mode →
//               FUSE mount & event loop (via libfuse low-level API).

#include "cmd/Cmd.hpp"
#include "fuse/Fs.hpp"
#include "utils/Logging.hpp"
#include "utils/Config.hpp"
#include <swordfs_version.h>

#define FUSE_USE_VERSION 31
#include <fuse_opt.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <folly/Format.h>

namespace swordfs::cmd::mount {

// ── Mount-specific option flags ────────────────────────────────────────

struct MountFlags {
  int help{0};
  int foreground{0};   // -f, --foreground: stay in foreground (default: daemonize)
  char* mountpoint{};
};

enum {
  kHelp       = 0,
  kForeground = 1,
  kVersion    = 2,
};

#define MOUNT_OPT(t, p, v) { t, offsetof(struct MountFlags, p), v }

static struct fuse_opt mount_opts[] = {
  MOUNT_OPT("-h",            help,       kHelp),
  MOUNT_OPT("--help",        help,       kHelp),
  MOUNT_OPT("-f",            foreground, kForeground),
  MOUNT_OPT("--foreground",  foreground, kForeground),
  FUSE_OPT_KEY("-V",         kVersion),
  FUSE_OPT_KEY("--version",  kVersion),
  FUSE_OPT_END
};

// ── Mountpoint validation ──────────────────────────────────────────────

static bool IsFuseMounted(const std::string& mp) {
  // Check /proc/mounts for an existing FUSE mount
  FILE* f = std::fopen("/proc/mounts", "r");
  if (!f) return false;
  char line[4096];
  while (std::fgets(line, sizeof(line), f)) {
    // Each line: device mountpoint fstype ...
    char dev[256], path[256], fstype[32];
    if (std::sscanf(line, "%255s %255s %31s", dev, path, fstype) == 3) {
      if (mp == path) {
        std::fclose(f);
        return true;
      }
    }
  }
  std::fclose(f);
  return false;
}

static bool IsStaleMount(const std::string& mp) {
  // Check if the mountpoint is listed but the FUSE connection is dead.
  // A simple heuristic: try to stat the mountpoint; if it returns ENOTCONN
  // the FUSE daemon is gone but the kernel still holds the mount.
  struct stat st;
  if (::stat(mp.c_str(), &st) == -1 && errno == ENOTCONN) {
    return true;
  }
  return false;
}

static int ValidateMountpoint(const std::string& mp) {
  // Refuse to mount at the root directory — would break the OS
  if (mp == "/") {
    SWORDFS_PROMPT << "Error: refusing to mount at root directory '/'. "
                      "Mounting at root would render the system unusable.";
    return -1;
  }

  struct stat st;

  // Create the mountpoint if it does not exist (like JuiceFS)
  if (::stat(mp.c_str(), &st) == -1) {
    if (errno == ENOENT) {
      if (::mkdir(mp.c_str(), 0755) == 0) {
        SWORDFS_LOG_INFO << "Created mountpoint '" << mp << "'";
        return 0;
      }
      SWORDFS_PROMPT << "Error: failed to create mountpoint '"
                     << mp << "': " << std::strerror(errno);
      return -1;
    }
    SWORDFS_PROMPT << "Error: cannot stat mountpoint '"
                   << mp << "': " << std::strerror(errno);
    return -1;
  }

  if (!S_ISDIR(st.st_mode)) {
    SWORDFS_PROMPT << "Error: mountpoint '" << mp << "' is not a directory";
    return -1;
  }

  // Must not already be mounted
  if (IsFuseMounted(mp)) {
    SWORDFS_PROMPT << "Error: mountpoint '" << mp << "' is already mounted";
    return -1;
  }

  // Detect stale mount
  if (IsStaleMount(mp)) {
    SWORDFS_PROMPT << "Warning: stale FUSE mount detected at '" << mp << "'. "
                      "You may need to run 'fusermount3 -u " << mp << "' first.";
  }

  return 0;
}

// ── Daemon mode ────────────────────────────────────────────────────────

static int Daemonize() {
  pid_t pid = ::fork();
  if (pid < 0) {
    SWORDFS_PROMPT << "Error: fork() failed: " << std::strerror(errno);
    return -1;
  }

  if (pid > 0) {
    // Parent: wait briefly for child to mount, then exit.
    // The child will exit if mount fails, parent detects via waitpid.
    int status;
    // Sleep a short time for child to either succeed (Daemonize) or fail (exit)
    ::sleep(1);
    pid_t w = ::waitpid(pid, &status, WNOHANG);
    if (w == pid) {
      // Child exited quickly → mount failed
      SWORDFS_PROMPT << "Error: daemon failed to mount";
      return -1;
    }
    ::_exit(0);
  }

  // Child: detach from terminal
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

  return 0;
}

// ── FUSE option processor callback ─────────────────────────────────────

static int MountOptProc(void* /*data*/, const char* /*arg*/, int key,
                        struct fuse_args* /*outargs*/) {
  if (key == kVersion) {
    fmt::print("SwordFS version {}.{}.{}\n",
               SWORDFS_VERSION_MAJOR, SWORDFS_VERSION_MINOR,
               SWORDFS_VERSION_PATCH);
    return -1;
  }
  return 1;
}

// ── Command handler ────────────────────────────────────────────────────

int MountCmd(const ::swordfs::cmd::CmdArgs& args) {
  const char* prog = "swordfs";

  // ── Parse mount-specific options ─────────────────────────────────

  MountFlags flags{};
  struct fuse_args fargs = FUSE_ARGS_INIT(args.argc, args.argv);

  if (fuse_opt_parse(&fargs, &flags, mount_opts, MountOptProc) == -1) {
    fuse_opt_free_args(&fargs);
    return 1;
  }

  if (flags.help) {
    ::swordfs::cmd::CommandCenter::Instance().ShowHelp(prog, "mount");
    fuse_opt_free_args(&fargs);
    return 0;
  }

  // ── Extract mountpoint (last positional argument) ─────────────────

  std::string mountpoint;
  for (int i = 1; i < fargs.argc; ++i) {
    if (fargs.argv[i][0] != '-') {
      mountpoint = fargs.argv[i];
    }
  }

  if (mountpoint.empty()) {
    SWORDFS_PROMPT << "Error: missing mountpoint";
    ::swordfs::cmd::CommandCenter::Instance().ShowHelp(prog, "mount");
    fuse_opt_free_args(&fargs);
    return 1;
  }

  // ── Validate mountpoint ──────────────────────────────────────────

  if (ValidateMountpoint(mountpoint) != 0) {
    fuse_opt_free_args(&fargs);
    return 1;
  }

  // ── Daemonize ────────────────────────────────────────────────────

  // Default: fork to background (like libfuse fuse_main).
  // -f / --foreground is parsed globally and stored in Config;
  // mount_opts still consumes "-f" so it doesn't leak to libfuse.
  if (!::swordfs::config::ConfigCenter::Instance().foreground()) {
    if (Daemonize() != 0) {
      fuse_opt_free_args(&fargs);
      return 1;
    }
  }

  // ── Mount via libfuse low-level API ──────────────────────────────

  // Signal handling is managed by fuse_set_signal_handlers() inside
  // swordfs::fuse::Mount(). The low-level session loop will exit cleanly
  // on SIGINT/SIGTERM or when the filesystem is unmounted externally.
  //
  // Skip argv[0] (the subcommand name "mount") — only pass
  // remaining options + mountpoint to libfuse for layered parsing.
  int ret = ::swordfs::fuse::Mount(fargs.argc - 1, fargs.argv + 1);

  fuse_opt_free_args(&fargs);

  if (ret != 0) {
    SWORDFS_PROMPT << "Error: mount failed (code " << ret << ")";
  }

  return ret;
}

}  // namespace swordfs::cmd::mount

// ── Auto-registration ──────────────────────────────────────────────────

namespace {
struct AutoRegister {
  AutoRegister() {
    swordfs::cmd::CommandCenter::Instance().Register({
      .name = "mount",
      .description = "Mount a filesystem with SwordFS",
      .usage = "mount [options] <mountpoint>",
      .options = {
        {"-f, --foreground", "",   "Run in foreground (default: daemonize)"},
        {"-o,",              "opt[,opt...]", "FUSE mount options (see mount.fuse3(8))"},
        {"-h, --help",       "",   "Show this help message"},
        {"-V, --version",    "",   "Show version information"},
      },
      .examples = {
        {"# Mount in background (default)",  "swordfs mount /mnt/swordfs"},
        {"# Mount in foreground",            "swordfs mount -f /mnt/swordfs"},
        {"# Mount with FUSE options",        "swordfs mount -o allow_other,ro /mnt/swordfs"},
      },
      .handler = swordfs::cmd::mount::MountCmd,
    });
  }
};
static AutoRegister _;
}  // anonymous namespace
