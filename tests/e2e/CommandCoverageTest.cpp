// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <folly/ScopeGuard.h>
#include <gtest/gtest.h>
#include <linux/magic.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace swordfs::e2e {
namespace {

int WaitForProcess(pid_t pid) {
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return -1;
    }
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return -1;
}

pid_t SpawnProcess(std::vector<std::string> args) {
  const pid_t pid = ::fork();
  if (pid != 0) {
    return pid;
  }

  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (auto &arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  ::execv(argv.front(), argv.data());
  ::_exit(127);
}

int RunProcess(std::vector<std::string> args) {
  const pid_t pid = SpawnProcess(std::move(args));
  if (pid < 0) {
    return -1;
  }
  return WaitForProcess(pid);
}

bool IsFuseMounted(const std::string &path) {
  struct statfs st{};
  return ::statfs(path.c_str(), &st) == 0 && static_cast<unsigned long>(st.f_type) == FUSE_SUPER_MAGIC;
}

bool WaitForFuseMount(const std::string &path) {
  constexpr int kMaxRetries = 100;
  constexpr auto kDelay = std::chrono::milliseconds(50);
  for (int i = 0; i < kMaxRetries; ++i) {
    if (IsFuseMounted(path)) {
      return true;
    }
    std::this_thread::sleep_for(kDelay);
  }
  return false;
}

bool WaitForProcessExit(pid_t pid, int *exit_code) {
  constexpr int kMaxRetries = 100;
  constexpr auto kDelay = std::chrono::milliseconds(50);
  for (int i = 0; i < kMaxRetries; ++i) {
    int status = 0;
    const pid_t result = ::waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      if (exit_code != nullptr) {
        *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      }
      return true;
    }
    if (result < 0 && errno != EINTR) {
      return false;
    }
    std::this_thread::sleep_for(kDelay);
  }
  return false;
}

class CommandCoverageE2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char *bin = std::getenv("SWORDFS_BIN");
    const char *meta = std::getenv("SWORDFS_METADATA_URL");
    const char *bucket = std::getenv("SWORDFS_E2E_S3_BUCKET");
    ASSERT_NE(bin, nullptr);
    ASSERT_NE(meta, nullptr);
    ASSERT_NE(bucket, nullptr);
    ASSERT_NE(bin[0], '\0');
    ASSERT_NE(meta[0], '\0');
    ASSERT_NE(bucket[0], '\0');

    swordfs_bin_ = bin;
    metadata_url_ = meta;

    auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    const std::string test_name = std::string(info->test_suite_name()) + "_" + info->name();
    volume_ = "cmdcov" + std::to_string(::getpid()) + std::to_string(std::hash<std::string>{}(test_name));
    work_dir_ = "/tmp/swordfs_command_coverage_" + test_name;
    mountpoint_ = work_dir_ + "/mnt";
    log_path_ = work_dir_ + "/swordfs.log";

    bucket_url_ = bucket;
    if (!bucket_url_.empty() && bucket_url_.back() != '/') {
      bucket_url_ += '/';
    }
    bucket_url_ += volume_;

    std::error_code ec;
    std::filesystem::create_directories(work_dir_, ec);
    ASSERT_FALSE(ec) << ec.message();
  }

  void TearDown() override {
    if (mount_pid_ > 0) {
      if (IsFuseMounted(mountpoint_)) {
        (void)RunProcess({"/bin/sh", "-c", "fusermount3 -u " + mountpoint_ + " || fusermount3 -uz " + mountpoint_});
      }
      int exit_code = 0;
      if (!WaitForProcessExit(mount_pid_, &exit_code)) {
        ::kill(mount_pid_, SIGKILL);
        (void)::waitpid(mount_pid_, nullptr, 0);
      }
      mount_pid_ = -1;
    }

    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path("/etc/swordfs") / volume_, ec);
    std::filesystem::remove_all(work_dir_, ec);
  }

  std::vector<std::string> FormatArgs() const {
    return {
        swordfs_bin_, "--log-file", log_path_,     "format",   "--volume",
        volume_,      "--meta",     metadata_url_, "--bucket", bucket_url_,
    };
  }

  std::vector<std::string> MountArgs(std::string mountpoint, std::string volume, bool foreground = true,
                                     std::string pidfile = {}) const {
    std::vector<std::string> args{
        swordfs_bin_,
        "--log-file",
        log_path_,
        "mount",
    };
    if (foreground) {
      args.push_back("-f");
    }
    args.insert(args.end(), {"--volume", std::move(volume), "--meta", metadata_url_, "--fuse-threads", "2",
                             "--storage-thread-count", "2"});
    if (!pidfile.empty()) {
      args.insert(args.end(), {"--pidfile", std::move(pidfile)});
    }
    args.push_back(std::move(mountpoint));
    return args;
  }

  static bool WaitForPidGone(pid_t pid) {
    constexpr int kMaxRetries = 100;
    constexpr auto kDelay = std::chrono::milliseconds(50);
    for (int i = 0; i < kMaxRetries; ++i) {
      if (::kill(pid, 0) != 0 && errno == ESRCH) {
        return true;
      }
      std::this_thread::sleep_for(kDelay);
    }
    return false;
  }

  std::string swordfs_bin_;
  std::string metadata_url_;
  std::string bucket_url_;
  std::string volume_;
  std::string work_dir_;
  std::string mountpoint_;
  std::string log_path_;
  pid_t mount_pid_ = -1;
};

TEST_F(CommandCoverageE2ETest, MainHandlesNoCommandParseErrorsAndCoredumpEnvironment) {
  EXPECT_EQ(RunProcess({swordfs_bin_}), 0);
  EXPECT_NE(RunProcess({swordfs_bin_, "--definitely-invalid-option"}), 0);

  std::optional<std::string> previous_coredump;
  if (const char *value = std::getenv("SWORDFS_ENABLE_COREDUMP")) {
    previous_coredump = value;
  }
  ASSERT_EQ(::setenv("SWORDFS_ENABLE_COREDUMP", "1", 1), 0);
  EXPECT_EQ(RunProcess({swordfs_bin_}), 0);
  if (previous_coredump.has_value()) {
    ASSERT_EQ(::setenv("SWORDFS_ENABLE_COREDUMP", previous_coredump->c_str(), 1), 0);
  } else {
    ASSERT_EQ(::unsetenv("SWORDFS_ENABLE_COREDUMP"), 0);
  }
}

TEST_F(CommandCoverageE2ETest, FormatReportsExistingVolumeFailure) {
  ASSERT_EQ(RunProcess(FormatArgs()), 0);
  EXPECT_NE(RunProcess(FormatArgs()), 0);
}

TEST_F(CommandCoverageE2ETest, MountRejectsRootRegularFileAndUncreatableMountpoints) {
  EXPECT_NE(RunProcess(MountArgs("/", volume_)), 0);

  const std::string regular_file = work_dir_ + "/not-a-directory";
  {
    std::ofstream out(regular_file);
    ASSERT_TRUE(out);
  }
  EXPECT_NE(RunProcess(MountArgs(regular_file, volume_)), 0);

  auto dash_prefixed = MountArgs("-invalid-mountpoint", volume_);
  dash_prefixed.insert(dash_prefixed.end() - 1, "--");
  EXPECT_NE(RunProcess(std::move(dash_prefixed)), 0);

  const std::string uncreatable = "/proc/swordfs-command-coverage-" + std::to_string(::getpid()) + "/mnt";
  EXPECT_NE(RunProcess(MountArgs(uncreatable, volume_)), 0);
}

TEST_F(CommandCoverageE2ETest, ForegroundMountCreatesMountpointThenReportsMissingVolume) {
  ASSERT_FALSE(std::filesystem::exists(mountpoint_));
  EXPECT_NE(RunProcess(MountArgs(mountpoint_, volume_)), 0);
  EXPECT_TRUE(std::filesystem::is_directory(mountpoint_));
}

TEST_F(CommandCoverageE2ETest, ForegroundMountRejectsStaleFuseMountAfterDaemonCrash) {
  ASSERT_EQ(RunProcess(FormatArgs()), 0);
  std::filesystem::create_directories(mountpoint_);

  mount_pid_ = SpawnProcess(MountArgs(mountpoint_, volume_));
  ASSERT_GT(mount_pid_, 0);
  auto cleanup = folly::makeGuard([&] {
    if (mount_pid_ > 0) {
      (void)::kill(mount_pid_, SIGKILL);
      (void)::waitpid(mount_pid_, nullptr, 0);
      mount_pid_ = -1;
    }
    (void)RunProcess({"/bin/sh", "-c", "fusermount3 -uz " + mountpoint_ + " >/dev/null 2>&1 || true"});
  });
  ASSERT_TRUE(WaitForFuseMount(mountpoint_));

  ASSERT_EQ(::kill(mount_pid_, SIGKILL), 0);
  int exit_code = -1;
  ASSERT_TRUE(WaitForProcessExit(mount_pid_, &exit_code));
  mount_pid_ = -1;
  ASSERT_EQ(exit_code, 128 + SIGKILL);

  bool stale = false;
  for (int i = 0; i < 100; ++i) {
    struct stat st{};
    errno = 0;
    if (::stat(mountpoint_.c_str(), &st) != 0 && errno == ENOTCONN) {
      stale = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ASSERT_TRUE(stale);
  EXPECT_NE(RunProcess(MountArgs(mountpoint_, volume_)), 0);

  ASSERT_EQ(RunProcess({"/bin/sh", "-c", "fusermount3 -uz " + mountpoint_}), 0);
}

TEST_F(CommandCoverageE2ETest, ForegroundMountPropagatesLibfusePermissionFailure) {
  ASSERT_EQ(RunProcess(FormatArgs()), 0);
  std::filesystem::create_directories(mountpoint_);
  ASSERT_EQ(::chmod(mountpoint_.c_str(), 0000), 0);

  const pid_t pid = SpawnProcess(MountArgs(mountpoint_, volume_));
  ASSERT_GT(pid, 0);
  int exit_code = -1;
  const bool exited = WaitForProcessExit(pid, &exit_code);
  if (!exited) {
    if (IsFuseMounted(mountpoint_)) {
      (void)RunProcess({"/bin/sh", "-c", "fusermount3 -uz " + mountpoint_});
    }
    ::kill(pid, SIGKILL);
    (void)::waitpid(pid, nullptr, 0);
  }
  ASSERT_TRUE(exited);
  EXPECT_NE(exit_code, 0);
  ASSERT_EQ(::chmod(mountpoint_.c_str(), 0700), 0);
}

TEST_F(CommandCoverageE2ETest, ForegroundMountCompletesLifecycleAndRejectsSecondMount) {
  ASSERT_EQ(RunProcess(FormatArgs()), 0);
  std::filesystem::create_directories(mountpoint_);

  mount_pid_ = SpawnProcess(MountArgs(mountpoint_, volume_));
  ASSERT_GT(mount_pid_, 0);
  ASSERT_TRUE(WaitForFuseMount(mountpoint_));

  EXPECT_NE(RunProcess(MountArgs(mountpoint_, volume_)), 0);
  ASSERT_TRUE(IsFuseMounted(mountpoint_));

  ASSERT_EQ(RunProcess({"/bin/sh", "-c", "fusermount3 -u " + mountpoint_}), 0);

  int exit_code = -1;
  ASSERT_TRUE(WaitForProcessExit(mount_pid_, &exit_code));
  mount_pid_ = -1;
  EXPECT_EQ(exit_code, 0);
}

TEST_F(CommandCoverageE2ETest, DaemonMountReportsMissingVolume) {
  std::filesystem::create_directories(mountpoint_);
  const std::string pidfile = work_dir_ + "/missing-volume.pid";

  EXPECT_NE(RunProcess(MountArgs(mountpoint_, volume_, false, pidfile)), 0);
  EXPECT_FALSE(IsFuseMounted(mountpoint_));

  std::ifstream in(pidfile);
  pid_t daemon_pid = -1;
  if (in) {
    in >> daemon_pid;
  }
  if (daemon_pid > 0) {
    EXPECT_TRUE(WaitForPidGone(daemon_pid));
  }
}

TEST_F(CommandCoverageE2ETest, DaemonMountWritesPidfileAndCompletesLifecycle) {
  ASSERT_EQ(RunProcess(FormatArgs()), 0);
  std::filesystem::create_directories(mountpoint_);
  const std::string pidfile = work_dir_ + "/daemon.pid";

  ASSERT_EQ(RunProcess(MountArgs(mountpoint_, volume_, false, pidfile)), 0);
  ASSERT_TRUE(WaitForFuseMount(mountpoint_));

  std::ifstream in(pidfile);
  ASSERT_TRUE(in);
  pid_t daemon_pid = -1;
  in >> daemon_pid;
  ASSERT_GT(daemon_pid, 0);
  EXPECT_EQ(::kill(daemon_pid, 0), 0);

  ASSERT_EQ(RunProcess({"/bin/sh", "-c", "fusermount3 -u " + mountpoint_}), 0);
  EXPECT_TRUE(WaitForPidGone(daemon_pid));
}

}  // namespace
}  // namespace swordfs::e2e
