// SwordFS mount subcommand
//
// Usage: swordfs mount [options] <mountpoint>
//
// Modeled after JuiceFS cmd/mount.go and cmd/mount_unix.go.
// Orchestrates: argument parsing → mountpoint validation → daemon mode →
//               signal handling → FUSE mount & event loop.

#include "cmd/cmd.hpp"
#include "fuse/fs.hpp"
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
#include <folly/logging/xlog.h>

// ── Mount-specific option flags ────────────────────────────────────────

struct MountFlags {
  int help{0};
  int daemon{0};       // -d, --daemon: fork to background
  char* mountpoint{};
};

enum {
  kHelp    = 0,
  kDaemon  = 1,
  kVersion = 2,
};

#define MOUNT_OPT(t, p, v) { t, offsetof(struct MountFlags, p), v }

static struct fuse_opt mount_opts[] = {
  MOUNT_OPT("-h",            help,    kHelp),
  MOUNT_OPT("--help",        help,    kHelp),
  MOUNT_OPT("-d",            daemon,  kDaemon),
  MOUNT_OPT("--daemon",      daemon,  kDaemon),
  MOUNT_OPT("--background",  daemon,  kDaemon),
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
    fmt::print(stderr, "Error: refusing to mount at root directory '/'. "
                       "Mounting at root would render the system unusable.\n");
    return -1;
  }

  struct stat st;

  // Create the mountpoint if it does not exist (like JuiceFS)
  if (::stat(mp.c_str(), &st) == -1) {
    if (errno == ENOENT) {
      if (::mkdir(mp.c_str(), 0755) == 0) {
        XLOG(INFO, "Created mountpoint '", mp, "'");
        return 0;
      }
      fmt::print(stderr, "Error: failed to create mountpoint '{}': {}\n",
                 mp, std::strerror(errno));
      return -1;
    }
    fmt::print(stderr, "Error: cannot stat mountpoint '{}': {}\n",
               mp, std::strerror(errno));
    return -1;
  }

  // Must be a directory
  if (!S_ISDIR(st.st_mode)) {
    fmt::print(stderr, "Error: mountpoint '{}' is not a directory\n", mp);
    return -1;
  }

  // Must not already be mounted
  if (IsFuseMounted(mp)) {
    fmt::print(stderr, "Error: mountpoint '{}' is already mounted\n", mp);
    return -1;
  }

  // Detect stale mount
  if (IsStaleMount(mp)) {
    fmt::print(stderr, "Warning: stale FUSE mount detected at '{}'. "
                       "You may need to run 'fusermount3 -u {}' first.\n",
               mp, mp);
  }

  return 0;
}

// ── Daemon mode ────────────────────────────────────────────────────────

static int Daemonize() {
  pid_t pid = ::fork();
  if (pid < 0) {
    fmt::print(stderr, "Error: fork() failed: {}\n", std::strerror(errno));
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
      fmt::print(stderr, "Error: daemon failed to mount\n");
      return -1;
    }
    fmt::print("SwordFS mounted in background (pid {})\n", pid);
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

// ── Help ───────────────────────────────────────────────────────────────

static void ShowHelp(const char* prog) {
  fmt::print("usage: {} mount [options] <mountpoint>\n\n", prog);
  fmt::print("Mount a SwordFS filesystem.\n\n"
             "Examples:\n"
             "# Mount in foreground\n"
             "  {} mount /mnt/swordfs\n\n"
             "# Mount in background (daemon)\n"
             "  {} mount -d /mnt/swordfs\n\n"
             "# Mount with FUSE options\n"
             "  {} mount -o allow_other,ro /mnt/swordfs\n\n"
             "Options:\n"
             "  -d, --daemon      Run in background\n"
             "  -o opt[,opt...]   FUSE mount options (see mount.fuse3(8))\n"
             "  -h, --help        Show this help message\n"
             "  -V, --version     Show version information\n"
             "\n",
             prog, prog, prog);
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

// ── Signal handling ────────────────────────────────────────────────────

namespace {

// The signal handler does minimal work: just flag the event.
// The main FUSE loop will notice and exit cleanly.
static volatile sig_atomic_t g_signaled{0};

static void SignalHandler(int sig) {
  (void)sig;
  g_signaled = 1;
}

static void InstallSignalHandlers() {
  struct sigaction sa{};
  sa.sa_handler = SignalHandler;
  sa.sa_flags   = SA_RESTART; // restart interrupted syscalls
  ::sigemptyset(&sa.sa_mask);
  ::sigaction(SIGINT,  &sa, nullptr);
  ::sigaction(SIGTERM, &sa, nullptr);
  ::sigaction(SIGHUP,  &sa, nullptr);
}

} // anonymous namespace

// ── Command handler ────────────────────────────────────────────────────

static int MountCmd(const swordfs::cmd::CmdArgs& args) {
  const char* prog = "swordfs";

  // ── Parse mount-specific options ─────────────────────────────────

  MountFlags flags{};
  struct fuse_args fargs = FUSE_ARGS_INIT(args.argc, args.argv);

  if (fuse_opt_parse(&fargs, &flags, mount_opts, MountOptProc) == -1) {
    fuse_opt_free_args(&fargs);
    return 1;
  }

  if (flags.help) {
    ShowHelp(prog);
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
    fmt::print(stderr, "Error: missing mountpoint\n");
    ShowHelp(prog);
    fuse_opt_free_args(&fargs);
    return 1;
  }

  // ── Validate mountpoint ──────────────────────────────────────────

  if (ValidateMountpoint(mountpoint) != 0) {
    fuse_opt_free_args(&fargs);
    return 1;
  }

  // ── Daemonize ────────────────────────────────────────────────────

  if (flags.daemon) {
    if (Daemonize() != 0) {
      fuse_opt_free_args(&fargs);
      return 1;
    }
  }

  // ── Install signal handlers ──────────────────────────────────────

  InstallSignalHandlers();

  // ── Mount via libfuse ────────────────────────────────────────────

  int ret = swordfs::fuse::Mount(fargs.argc, fargs.argv);

  fuse_opt_free_args(&fargs);

  if (ret != 0) {
    fmt::print(stderr, "Error: mount failed (code {})\n", ret);
  }

  return ret;
}

// ── Auto-registration ──────────────────────────────────────────────────

namespace {
struct AutoRegister {
  AutoRegister() {
    swordfs::cmd::RegisterCommand({
      .name = "mount",
      .description = "Mount a filesystem via FUSE",
      .handler = MountCmd,
    });
  }
};
static AutoRegister _;
} // anonymous namespace
