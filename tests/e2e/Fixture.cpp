// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "tests/e2e/Fixture.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <folly/FileUtil.h>
#include <folly/hash/Hash.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <thread>

#include "metadata/IMetaEngine.hpp"
#include "metadata/MetaEngineRegistry.hpp"
#include "metadata/Utils.hpp"

namespace swordfs {
namespace e2e {

// ────────────────────────────────────────────────────────────────
// Construction / Destruction
// ────────────────────────────────────────────────────────────────

Fixture::Fixture() {
  // SWORDFS_E2E_S3_BUCKET is required for all E2E tests.
  // Defer failure to SetUp() where GTest macros are available.
  const char *bucket = std::getenv("SWORDFS_E2E_S3_BUCKET");
  bucket_url_ = bucket ? bucket : "";

  // Ensure trailing slash so the test-specific prefix (appended in
  // SetUp) becomes a proper S3 key prefix.
  if (!bucket_url_.empty() && bucket_url_.back() != '/') {
    bucket_url_ += '/';
  }
  // Save the base URL so SetUp() can be called multiple times
  // without accumulating test-name suffixes.
  base_bucket_url_ = bucket_url_;
}

Fixture::~Fixture() {
}

// ────────────────────────────────────────────────────────────────
// Lifecycle
// ────────────────────────────────────────────────────────────────

bool Fixture::SetUp() {
  if (base_bucket_url_.empty()) {
    std::fprintf(stderr,
                 "E2E: SWORDFS_E2E_S3_BUCKET is not set or empty. "
                 "E2E tests require a valid S3 bucket URL.\n");
    return false;
  }

  InitPaths();

  std::error_code ec;
  std::filesystem::create_directories(work_dir_, ec);
  if (ec) {
    std::fprintf(stderr, "E2E: failed to create %s: %s\n", work_dir_.c_str(), ec.message().c_str());
    return false;
  }

  if (!volume_formatted_) {
    if (!FormatVolume()) {
      RemoveVolumeConfig();
      return false;
    }
    volume_formatted_ = true;
  }
  if (!StartMount()) {
    RemoveVolumeConfig();
    return false;
  }
  if (!WaitForMount()) {
    StopMount();
    RemoveVolumeConfig();
    return false;
  }

  return true;
}

void Fixture::TearDown() {
  StopMount();
  RemoveVolumeConfig();

  if (!std::getenv("SWORDFS_E2E_KEEP_WORKDIR")) {
    std::string cmd = "rm -rf " + work_dir_;
    std::system(cmd.c_str());
  }
}

// ────────────────────────────────────────────────────────────────
// Format
// ────────────────────────────────────────────────────────────────

bool Fixture::FormatVolume() {
  const char *metadata_url = std::getenv("SWORDFS_METADATA_URL");
  if (!metadata_url || metadata_url[0] == '\0') {
    std::fprintf(stderr, "E2E: SWORDFS_METADATA_URL must be set for Redis metadata E2E\n");
    return false;
  }

  // Shell out to the `swordfs format` binary to exercise the real
  // CLI path end-to-end.
  std::ostringstream cmd;
  cmd << FindSwordfsBin() << " --log-file " << LogPath() << " format"
      << " --volume " << volume_name_ << " --meta " << metadata_url << " --bucket " << bucket_url_ << " 2>&1";

  int ret = std::system(cmd.str().c_str());
  if (ret != 0) {
    std::fprintf(stderr, "E2E: format failed (exit=%d): %s\n", WEXITSTATUS(ret), cmd.str().c_str());
    return false;
  }
  return true;
}

// ────────────────────────────────────────────────────────────────
// Mount
// ────────────────────────────────────────────────────────────────

bool Fixture::StartMount() {
  const char *metadata_url = std::getenv("SWORDFS_METADATA_URL");
  if (!metadata_url || metadata_url[0] == '\0') {
    std::fprintf(stderr, "E2E: SWORDFS_METADATA_URL must be set for Redis metadata E2E\n");
    return false;
  }

  std::error_code ec;
  std::filesystem::create_directories(mountpoint_, ec);
  if (ec) {
    std::fprintf(stderr, "E2E: failed to create %s: %s\n", mountpoint_.c_str(), ec.message().c_str());
    return false;
  }

  std::ostringstream cmd;
  cmd << FindSwordfsBin() << " --log-file " << LogPath() << " mount "
      << " --volume " << volume_name_ << " --meta " << metadata_url << " --fuse-threads 2"
      << " --storage-thread-count 2"
      << " --pidfile " << work_dir_ << "/swordfs.pid"
      << " " << mountpoint_ << " 2>&1";

  int ret = std::system(cmd.str().c_str());
  if (ret != 0) {
    std::fprintf(stderr, "E2E: mount failed (exit=%d): %s\n", WEXITSTATUS(ret), cmd.str().c_str());
    return false;
  }

  // Read the daemon PID written by --pidfile.  We cannot use
  // getpid() here because system() forks; the daemon runs as a
  // descendant of that fork, not as the test process.
  std::string pidfile = work_dir_ + "/swordfs.pid";
  std::ifstream pfs(pidfile);
  if (pfs) {
    pfs >> daemon_pid_;
  }

  return true;
}

bool Fixture::WaitForMount() {
  constexpr int kMaxRetries = 100;
  constexpr useconds_t kDelayUs = 50000;  // 50 ms

  for (int i = 0; i < kMaxRetries; ++i) {
    if (IsMounted()) {
      mounted_ = true;
      return true;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(kDelayUs));
  }

  std::fprintf(stderr, "E2E: timed out waiting for mount at %s\n", mountpoint_.c_str());
  return false;
}

bool Fixture::StopMount() {
  if (!mounted_) {
    return true;
  }
  std::string cmd = "fusermount3 -u " + mountpoint_;
  if (std::system(cmd.c_str()) != 0) {
    // Force-unmount as fallback.
    cmd = "fusermount3 -uz " + mountpoint_;
    if (std::system(cmd.c_str()) != 0) {
      std::fprintf(stderr, "E2E: failed to unmount %s\n", mountpoint_.c_str());
      return false;
    }
  }
  mounted_ = false;
  return true;
}

bool Fixture::IsDaemonGone() const {
  if (daemon_pid_ <= 0) {
    return false;
  }
  // fusermount3 -u returns before the daemon has fully exited;
  // retry for up to 500 ms.
  for (int i = 0; i < 10; ++i) {
    if (kill(daemon_pid_, 0) != 0 && errno == ESRCH) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

// ────────────────────────────────────────────────────────────────
// POSIX wrappers
// ────────────────────────────────────────────────────────────────

int Fixture::Stat(const std::string &relpath, struct stat *st) const {
  if (::stat(MountPath(relpath).c_str(), st) != 0) {
    std::fprintf(stderr, "E2E: stat(%s) failed: %s\n", relpath.c_str(), std::strerror(errno));
    return -1;
  }
  return 0;
}

int Fixture::Lstat(const std::string &relpath, struct stat *st) const {
  if (::lstat(MountPath(relpath).c_str(), st) != 0) {
    std::fprintf(stderr, "E2E: lstat(%s) failed: %s\n", relpath.c_str(), std::strerror(errno));
    return -1;
  }
  return 0;
}

int Fixture::Statfs(struct statvfs *sv) const {
  if (::statvfs(mountpoint_.c_str(), sv) != 0) {
    return -1;
  }
  return 0;
}

int Fixture::Access(const std::string &relpath, int mode) {
  if (::access(MountPath(relpath).c_str(), mode) != 0) {
    std::fprintf(stderr, "E2E: access(%s) failed: %s\n", relpath.c_str(), std::strerror(errno));
    return -1;
  }
  return 0;
}

int Fixture::CreateFile(const std::string &relpath, mode_t mode, int flags) {
  int fd = ::open(MountPath(relpath).c_str(), flags, mode);
  if (fd < 0) {
    std::fprintf(stderr, "E2E: open(%s) failed: %s\n", relpath.c_str(), std::strerror(errno));
    return -1;
  }
  ::close(fd);
  return 0;
}

int Fixture::OpenFile(const std::string &relpath, int flags) {
  int fd = ::open(MountPath(relpath).c_str(), flags);
  if (fd < 0) {
    std::fprintf(stderr, "E2E: open(%s) failed: %s\n", relpath.c_str(), std::strerror(errno));
    return -1;
  }
  return fd;
}

int Fixture::ReadFile(const std::string &relpath, std::string *out) {
  if (!folly::readFile(MountPath(relpath).c_str(), *out)) {
    std::fprintf(stderr, "E2E: read(%s) failed: %s\n", relpath.c_str(), std::strerror(errno));
    return -1;
  }
  return 0;
}

int Fixture::WriteFile(const std::string &relpath, const std::string &data) {
  std::string path = MountPath(relpath);
  if (::access(path.c_str(), F_OK) != 0) {
    std::fprintf(stderr, "E2E: write(%s) failed: %s\n", relpath.c_str(), std::strerror(errno));
    return -1;
  }
  if (!folly::writeFile(data, path.c_str())) {
    std::fprintf(stderr, "E2E: write(%s) failed: %s\n", relpath.c_str(), std::strerror(errno));
    return -1;
  }
  return 0;
}

int Fixture::ReadDir(const std::string &relpath, std::vector<std::string> *entries) {
  entries->clear();
  DIR *dp = ::opendir(MountPath(relpath).c_str());
  if (!dp) {
    std::fprintf(stderr, "E2E: opendir(%s) failed: %s\n", relpath.c_str(), std::strerror(errno));
    return -1;
  }
  struct dirent *de;
  while ((de = ::readdir(dp)) != nullptr) {
    if (std::string(de->d_name) == "." || std::string(de->d_name) == "..") {
      continue;
    }
    entries->push_back(de->d_name);
  }
  ::closedir(dp);
  return 0;
}

int Fixture::UnlinkFile(const std::string &relpath) {
  if (::unlink(MountPath(relpath).c_str()) != 0) {
    std::fprintf(stderr, "E2E: unlink(%s) failed: %s\n", relpath.c_str(), std::strerror(errno));
    return -1;
  }
  return 0;
}

int Fixture::MkDir(const std::string &relpath, mode_t mode) {
  if (::mkdir(MountPath(relpath).c_str(), mode) != 0) {
    std::fprintf(stderr, "E2E: mkdir(%s) failed: %s\n", relpath.c_str(), std::strerror(errno));
    return -1;
  }
  return 0;
}

int Fixture::RmDir(const std::string &relpath) {
  if (::rmdir(MountPath(relpath).c_str()) != 0) {
    std::fprintf(stderr, "E2E: rmdir(%s) failed: %s\n", relpath.c_str(), std::strerror(errno));
    return -1;
  }
  return 0;
}

int Fixture::Rename(const std::string &oldpath, const std::string &newpath) {
  if (::rename(MountPath(oldpath).c_str(), MountPath(newpath).c_str()) != 0) {
    std::fprintf(stderr, "E2E: rename(%s -> %s) failed: %s\n", oldpath.c_str(), newpath.c_str(), std::strerror(errno));
    return -1;
  }
  return 0;
}

int Fixture::Symlink(const std::string &target, const std::string &linkpath) {
  if (::symlink(target.c_str(), MountPath(linkpath).c_str()) != 0) {
    std::fprintf(stderr, "E2E: symlink(%s -> %s) failed: %s\n", linkpath.c_str(), target.c_str(), std::strerror(errno));
    return -1;
  }
  return 0;
}

int Fixture::HardLink(const std::string &oldpath, const std::string &newpath) {
  if (::link(MountPath(oldpath).c_str(), MountPath(newpath).c_str()) != 0) {
    std::fprintf(stderr, "E2E: link(%s -> %s) failed: %s\n", oldpath.c_str(), newpath.c_str(), std::strerror(errno));
    return -1;
  }
  return 0;
}

int Fixture::Readlink(const std::string &relpath, std::string *target) {
  char buf[PATH_MAX];
  ssize_t len = ::readlink(MountPath(relpath).c_str(), buf, sizeof(buf) - 1);
  if (len < 0) {
    std::fprintf(stderr, "E2E: readlink(%s) failed: %s\n", relpath.c_str(), std::strerror(errno));
    return -1;
  }
  buf[len] = '\0';
  target->assign(buf, static_cast<size_t>(len));
  return 0;
}

int Fixture::Chmod(const std::string &relpath, mode_t mode) {
  if (::chmod(MountPath(relpath).c_str(), mode) != 0) {
    std::fprintf(stderr, "E2E: chmod(%s) failed: %s\n", relpath.c_str(), std::strerror(errno));
    return -1;
  }
  return 0;
}

int Fixture::Truncate(const std::string &relpath, off_t length) {
  if (::truncate(MountPath(relpath).c_str(), length) != 0) {
    std::fprintf(stderr, "E2E: truncate(%s) failed: %s\n", relpath.c_str(), std::strerror(errno));
    return -1;
  }
  return 0;
}

// ────────────────────────────────────────────────────────────────
// Test utilities
// ────────────────────────────────────────────────────────────────

bool Fixture::IsMounted() {
  // Verify the mountpoint is actually a FUSE filesystem, not just
  // a regular directory on the host.
  struct statvfs sv{};
  if (Statfs(&sv) != 0) {
    return false;
  }
  return sv.f_type == FUSE_SUPER_MAGIC;
}

std::string Fixture::MountPath(const std::string &relpath) const {
  if (relpath.empty() || relpath == ".") {
    return mountpoint_;
  }
  return mountpoint_ + "/" + relpath;
}

::testing::AssertionResult Fixture::UmaskEquals(const std::string &relpath, mode_t expected_mask) const {
  struct stat st;
  if (::stat(MountPath(relpath).c_str(), &st) != 0) {
    return ::testing::AssertionFailure() << "stat(" << relpath << ") failed: " << std::strerror(errno);
  }
  mode_t old = ::umask(0);
  ::umask(old);
  mode_t expected = expected_mask & ~old;
  if ((st.st_mode & 0777) != expected) {
    return ::testing::AssertionFailure() << "umask mismatch for " << relpath << ": expected " << std::oct << expected
                                         << ", got " << (st.st_mode & 0777);
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult Fixture::FileEquals(const std::string &relpath, size_t expected_size,
                                               uint64_t expected_hash) {
  std::string actual;
  if (ReadFile(relpath, &actual) != 0) {
    return ::testing::AssertionFailure() << "Failed to read \"" << relpath << "\": " << std::strerror(errno)
                                         << " (errno=" << errno << ")";
  }
  uint64_t actual_hash = Hash64(actual);
  if (actual.size() != expected_size || actual_hash != expected_hash) {
    return ::testing::AssertionFailure() << "File mismatch for \"" << relpath << "\""
                                         << "\n  Expected: size=" << expected_size << " hash=0x" << std::hex
                                         << expected_hash << "\n  Actual:   size=" << std::dec << actual.size()
                                         << " hash=0x" << std::hex << actual_hash;
  }
  return ::testing::AssertionSuccess();
}

uint64_t Fixture::Hash64(std::string_view data) {
  return folly::hash::SpookyHashV2::Hash64(data.data(), data.size(), 0);
}

std::string Fixture::GenerateRandomData(size_t len, RandomMode mode) {
  thread_local std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<unsigned char> byte_dist;

  if (mode == RandomMode::kUpTo) {
    std::uniform_int_distribution<size_t> len_dist(1, len);
    len = len_dist(rng);
  }

  std::string data(len, '\0');
  for (size_t i = 0; i < len; ++i) {
    data[i] = static_cast<char>(byte_dist(rng));
  }
  return data;
}

metadata::Limits Fixture::GetLimits() const {
  const char *metadata_url = std::getenv("SWORDFS_METADATA_URL");
  const auto url = metadata_url && metadata_url[0] != '\0' ? metadata_url : "memory://local";

  std::unique_ptr<metadata::IMetaEngine> engine;
  std::string scheme;
  auto status = metadata::ParseUrlScheme(url, &scheme);
  if (status.ok()) {
    status = metadata::MetaEngineRegistry::Instance().Create(scheme, url, volume_name_, &engine);
  }
  if (!status.ok()) {
    ADD_FAILURE() << "Failed to create metadata engine: " << status.message();
    return {};
  }
  return engine->GetLimits();
}

// ────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────

std::string Fixture::FindSwordfsBin() const {
  const char *env = std::getenv("SWORDFS_BIN");
  if (env && env[0] != '\0') {
    return env;
  }

  // Default: look alongside the test binary in the build directory.
  // The test binary is at build/swordfs_e2e_test, swordfs is at build/swordfs.
  return "./swordfs";
}

std::string Fixture::LogPath() const {
  const char *log = std::getenv("SWORDFS_E2E_LOG");
  return log ? log : work_dir_ + "/swordfs.log";
}

void Fixture::RemoveVolumeConfig() {
  constexpr std::string_view kConfigRoot = "/etc/swordfs";
  const std::filesystem::path volume_dir = std::filesystem::path(kConfigRoot) / volume_name_;
  std::error_code ec;
  std::filesystem::remove_all(volume_dir, ec);
  if (ec) {
    std::fprintf(stderr, "E2E: failed to remove volume config %s: %s\n", volume_dir.c_str(), ec.message().c_str());
  }
}

void Fixture::InitPaths() {
  // Use the fully-qualified gtest name (suite_test) as the unique
  // namespace so that parallel / repeated runs never collide.
  auto *test_info = ::testing::UnitTest::GetInstance()->current_test_info();
  const std::string test_name = std::string(test_info->test_suite_name()) + "_" + test_info->name();

  // Volume names are intentionally restricted to ASCII letters and digits.
  // Keep the human-readable test name for paths, but derive a valid unique
  // volume name from it by removing separators and appending a stable hash.
  volume_name_ = test_name;
  volume_name_.erase(std::remove(volume_name_.begin(), volume_name_.end(), '_'), volume_name_.end());
  volume_name_ += std::to_string(folly::hash::SpookyHashV2::Hash64(test_name.data(), test_name.size(), 0));
  work_dir_ = "/tmp/swordfs_e2e_" + test_name;
  mountpoint_ = work_dir_ + "/mnt";
  // Restore base URL and append test-specific prefix.
  bucket_url_ = base_bucket_url_ + volume_name_;
}

}  // namespace e2e
}  // namespace swordfs
