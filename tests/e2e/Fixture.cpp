// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "tests/e2e/Fixture.hpp"

#include <folly/FileUtil.h>
#include <folly/portability/Filesystem.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include <gtest/gtest.h>

namespace swordfs {
namespace e2e {

// ────────────────────────────────────────────────────────────────
// Construction / Destruction
// ────────────────────────────────────────────────────────────────

Fixture::Fixture() {
  // Derive S3 bucket from environment or use a default.
  const char* env_bucket = std::getenv("SWORDFS_E2E_S3_BUCKET");
  if (env_bucket && env_bucket[0] != '\0') {
    bucket_url_ = env_bucket;
  } else {
    // Default: MinIO on localhost with a test bucket.
    bucket_url_ = "s3://localhost:9000/swordfs-e2e";
  }
  // Ensure trailing slash so the test-specific prefix (appended in
  // SetUp) becomes a proper S3 key prefix.
  if (bucket_url_.back() != '/') {
    bucket_url_ += '/';
  }
}

Fixture::~Fixture() {
  if (mounted_) {
    TearDown();
  }
}

// ────────────────────────────────────────────────────────────────
// Lifecycle
// ────────────────────────────────────────────────────────────────

bool Fixture::SetUp() {
  // Use the fully-qualified gtest name (suite_test) as the unique
  // namespace so that parallel / repeated runs never collide.
  auto* test_info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string test_name =
      std::string(test_info->test_suite_name()) + "_" + test_info->name();

  volume_name_ = "e2e_" + test_name;
  work_dir_ = "/tmp/swordfs_e2e_" + test_name;
  mountpoint_ = work_dir_ + "/mnt";
  vol_config_dir_ = work_dir_ + "/config";

  // Append test-specific prefix to the bucket URL.
  bucket_url_ += test_name;

  // Create directory structure (parent directories are created automatically).
  std::error_code ec;
  folly::fs::create_directories(mountpoint_, ec);
  if (ec) {
    std::fprintf(stderr, "E2E: failed to create %s: %s\n",
                 mountpoint_.c_str(), ec.message().c_str());
    return false;
  }
  folly::fs::create_directories(vol_config_dir_, ec);
  if (ec) {
    std::fprintf(stderr, "E2E: failed to create %s: %s\n",
                 vol_config_dir_.c_str(), ec.message().c_str());
    return false;
  }

  if (!FormatVolume()) return false;
  if (!StartMount()) return false;
  if (!WaitForMount()) return false;

  mounted_ = true;
  return true;
}

void Fixture::TearDown() {
  if (mounted_) {
    StopMount();
    mounted_ = false;
  }

  // Best-effort cleanup of temp directory.
  std::string cmd = "rm -rf " + work_dir_ + " 2>/dev/null";
  std::system(cmd.c_str());

  work_dir_.clear();
  mountpoint_.clear();
  vol_config_dir_.clear();
}

// ────────────────────────────────────────────────────────────────
// Format
// ────────────────────────────────────────────────────────────────

bool Fixture::FormatVolume() {
  // Shell out to the `swordfs format` binary to exercise the real
  // CLI path end-to-end.

  std::ostringstream cmd;
  cmd << FindSwordfsBin()
      << " --log-file " << work_dir_ << "/swordfs.log"
      << " format"
      << " --volume " << volume_name_
      << " --meta memory://local"
      << " --bucket " << bucket_url_
      << " --volume-config-path " << vol_config_dir_
      << " 2>&1";

  int ret = std::system(cmd.str().c_str());
  if (ret != 0) {
    std::fprintf(stderr, "E2E: format failed (exit=%d): %s\n",
                 WEXITSTATUS(ret), cmd.str().c_str());
    return false;
  }
  return true;
}

// ────────────────────────────────────────────────────────────────
// Mount
// ────────────────────────────────────────────────────────────────

bool Fixture::StartMount() {
  std::ostringstream cmd;
  cmd << FindSwordfsBin()
      << " --log-file " << work_dir_ << "/swordfs.log"
      << " mount "
      << mountpoint_
      << " --volume-config-path " << vol_config_dir_
      << " --fuse-threads 2"
      << " --storage-async-threads 2"
      << " 2>&1 &";

  int ret = std::system(cmd.str().c_str());
  if (ret != 0) {
    std::fprintf(stderr, "E2E: mount failed (exit=%d): %s\n",
                 WEXITSTATUS(ret), cmd.str().c_str());
    return false;
  }
  return true;
}

bool Fixture::WaitForMount() {
  constexpr int kMaxRetries = 100;
  constexpr useconds_t kDelayUs = 50000;  // 50 ms

  for (int i = 0; i < kMaxRetries; ++i) {
    struct statvfs sv;
    if (::statvfs(mountpoint_.c_str(), &sv) == 0) {
      return true;
    }

    std::ifstream mounts("/proc/mounts");
    std::string line;
    while (std::getline(mounts, line)) {
      if (line.find(mountpoint_) != std::string::npos &&
          line.find("fuse") != std::string::npos) {
        return true;
      }
    }

    std::this_thread::sleep_for(std::chrono::microseconds(kDelayUs));
  }

  std::fprintf(stderr, "E2E: timed out waiting for mount at %s\n",
               mountpoint_.c_str());
  return false;
}

bool Fixture::StopMount() {
  // fusermount3 sends FUSE_DESTROY; the daemon exits on its own.
  std::string cmd = "fusermount3 -u " + mountpoint_ + " 2>/dev/null";
  std::system(cmd.c_str());
  return true;
}

// ────────────────────────────────────────────────────────────────
// Filesystem helpers
// ────────────────────────────────────────────────────────────────

std::string Fixture::MountPath(const std::string& relpath) const {
  if (relpath.empty()) return mountpoint_;
  return mountpoint_ + "/" + relpath;
}

bool Fixture::IsMounted() const {
  if (!mounted_) return false;
  struct statvfs sv;
  return ::statvfs(mountpoint_.c_str(), &sv) == 0;
}

bool Fixture::Mkdir(const std::string& relpath, mode_t mode) {
  std::error_code ec;
  if (!folly::fs::create_directory(MountPath(relpath), ec)) {
    return false;
  }
  folly::fs::permissions(MountPath(relpath),
                          static_cast<folly::fs::perms>(mode), ec);
  return !ec;
}

bool Fixture::Rmdir(const std::string& relpath) {
  std::error_code ec;
  folly::fs::remove(MountPath(relpath), ec);
  return !ec;
}

bool Fixture::Unlink(const std::string& relpath) {
  std::error_code ec;
  return folly::fs::remove(MountPath(relpath), ec) && !ec;
}

bool Fixture::Rename(const std::string& oldpath,
                         const std::string& newpath) {
  std::error_code ec;
  folly::fs::rename(MountPath(oldpath), MountPath(newpath), ec);
  return !ec;
}

struct stat Fixture::Stat(const std::string& relpath) {
  struct stat st {};
  std::string path = MountPath(relpath);
  if (::stat(path.c_str(), &st) != 0) {
    std::memset(&st, 0, sizeof(st));
  }
  return st;
}

struct statvfs Fixture::Statfs(const std::string& relpath) {
  struct statvfs sv {};
  std::string path = MountPath(relpath.empty() ? "" : relpath);
  ::statvfs(path.c_str(), &sv);
  return sv;
}

int Fixture::Access(const std::string& relpath, int mode) {
  return ::access(MountPath(relpath).c_str(), mode);
}

std::string Fixture::ReadFile(const std::string& relpath) {
  std::string data;
  if (!folly::readFile(MountPath(relpath).c_str(), data)) {
    return {};
  }
  return data;
}

bool Fixture::WriteFile(const std::string& relpath,
                            const std::string& data) {
  // Pin umask to 022 so newly-created files get 0644 regardless of
  // the caller's environment.
  mode_t old_umask = ::umask(022);
  folly::writeFile(data, MountPath(relpath).c_str());
  ::umask(old_umask);
  return true;
}

std::vector<std::string> Fixture::ReadDir(const std::string& relpath) {
  std::vector<std::string> entries;
  std::error_code ec;
  for (auto& entry : folly::fs::directory_iterator(MountPath(relpath), ec)) {
    std::string name = entry.path().filename().string();
    if (name != "." && name != "..") {
      entries.push_back(name);
    }
  }
  return entries;
}

bool Fixture::Truncate(const std::string& relpath, off_t size) {
  std::error_code ec;
  folly::fs::resize_file(MountPath(relpath), static_cast<uintmax_t>(size), ec);
  return !ec;
}

bool Fixture::Chmod(const std::string& relpath, mode_t mode) {
  std::error_code ec;
  folly::fs::permissions(MountPath(relpath),
                          static_cast<folly::fs::perms>(mode), ec);
  return !ec;
}

bool Fixture::Link(const std::string& target,
                       const std::string& linkpath) {
  std::error_code ec;
  folly::fs::create_hard_link(MountPath(target), MountPath(linkpath), ec);
  return !ec;
}

bool Fixture::Symlink(const std::string& target,
                          const std::string& linkpath) {
  std::error_code ec;
  folly::fs::create_symlink(target, MountPath(linkpath), ec);
  return !ec;
}

// ────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────

std::string Fixture::FindSwordfsBin() const {
  const char* env = std::getenv("SWORDFS_BIN");
  if (env && env[0] != '\0') return env;

  // Default: look alongside the test binary in the build directory.
  // The test binary is at build/swordfs_e2e_test, swordfs is at build/swordfs.
  return "./swordfs";
}

}  // namespace e2e
}  // namespace swordfs
