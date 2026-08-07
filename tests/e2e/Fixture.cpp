// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "tests/e2e/Fixture.hpp"

#include <folly/FileUtil.h>
#include <folly/hash/Hash.h>
#include <folly/portability/Filesystem.h>
#include <gtest/gtest.h>
#include <linux/magic.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

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

  if (!FormatVolume()) return false;
  if (!StartMount()) return false;
  if (!WaitForMount()) return false;

  return true;
}

void Fixture::TearDown() {
  StopMount();

  if (!std::getenv("SWORDFS_E2E_KEEP_WORKDIR")) {
    std::string cmd = "rm -rf " + work_dir_;
    std::system(cmd.c_str());
  }
}

// ────────────────────────────────────────────────────────────────
// Format
// ────────────────────────────────────────────────────────────────

bool Fixture::FormatVolume() {
  std::error_code ec;
  folly::fs::create_directories(vol_config_dir_, ec);
  if (ec) {
    std::fprintf(stderr, "E2E: failed to create %s: %s\n",
                 vol_config_dir_.c_str(), ec.message().c_str());
    return false;
  }

  // Shell out to the `swordfs format` binary to exercise the real
  // CLI path end-to-end.
  std::ostringstream cmd;
  cmd << FindSwordfsBin()
      << " --log-file " << LogPath()
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
  std::error_code ec;
  folly::fs::create_directories(mountpoint_, ec);
  if (ec) {
    std::fprintf(stderr, "E2E: failed to create %s: %s\n",
                 mountpoint_.c_str(), ec.message().c_str());
    return false;
  }

  std::ostringstream cmd;
  cmd << FindSwordfsBin()
      << " --log-file " << LogPath()
      << " mount "
      << " --volume " << volume_name_
      << " --meta memory://local"
      << " --volume-config-path " << vol_config_dir_
      << " --fuse-threads 2"
      << " --storage-async-threads 2"
      << " " << mountpoint_
      << " 2>&1";

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
    if (IsMounted()) {
      mounted_ = true;
      return true;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(kDelayUs));
  }

  std::fprintf(stderr, "E2E: timed out waiting for mount at %s\n",
               mountpoint_.c_str());
  return false;
}

bool Fixture::StopMount() {
  if (!mounted_) return true;
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

// ────────────────────────────────────────────────────────────────
// Filesystem helpers
// ────────────────────────────────────────────────────────────────

std::string Fixture::MountPath(const std::string &relpath) const {
  if (relpath.empty() || relpath == ".") return mountpoint_;
  return mountpoint_ + "/" + relpath;
}

bool Fixture::IsMounted() {
  // Verify the mountpoint is actually a FUSE filesystem, not just
  // a regular directory on the host.
  struct statvfs sv = Statfs();
  return sv.f_type == FUSE_SUPER_MAGIC;
}

struct statvfs Fixture::Statfs() {
  struct statvfs sv{};
  ::statvfs(mountpoint_.c_str(), &sv);
  return sv;
}

int Fixture::ReadFile(const std::string &relpath, std::string *out) {
  if (!folly::readFile(MountPath(relpath).c_str(), *out)) {
    return -1;
  }
  return 0;
}

int Fixture::WriteFile(const std::string &relpath,
                       const std::string &data) {
  if (!folly::writeFile(data, MountPath(relpath).c_str())) {
    return -1;
  }
  return 0;
}

::testing::AssertionResult Fixture::FileEquals(
    const std::string &relpath, size_t expected_size,
    uint64_t expected_hash) {
  std::string actual;
  if (ReadFile(relpath, &actual) != 0) {
    return ::testing::AssertionFailure()
           << "Failed to read \"" << relpath << "\": "
           << std::strerror(errno) << " (errno=" << errno << ")";
  }
  uint64_t actual_hash = Hash64(actual);
  if (actual.size() != expected_size || actual_hash != expected_hash) {
    return ::testing::AssertionFailure()
           << "File mismatch for \"" << relpath << "\""
           << "\n  Expected: size=" << expected_size << " hash=0x" << std::hex << expected_hash
           << "\n  Actual:   size=" << std::dec << actual.size() << " hash=0x" << std::hex << actual_hash;
  }
  return ::testing::AssertionSuccess();
}

uint64_t Fixture::Hash64(std::string_view data) {
  return folly::hash::SpookyHashV2::Hash64(data.data(), data.size(), 0);
}

int Fixture::ReadDir(const std::string &relpath, std::vector<std::string> *entries) {
  std::error_code ec;
  for (auto &entry : folly::fs::directory_iterator(MountPath(relpath), ec)) {
    std::string name = entry.path().filename().string();
    if (name != "." && name != "..") {
      entries->push_back(name);
    }
  }
  return ec ? -1 : 0;
}

// ────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────

std::string Fixture::FindSwordfsBin() const {
  const char *env = std::getenv("SWORDFS_BIN");
  if (env && env[0] != '\0') return env;

  // Default: look alongside the test binary in the build directory.
  // The test binary is at build/swordfs_e2e_test, swordfs is at build/swordfs.
  return "./swordfs";
}

std::string Fixture::LogPath() const {
  const char *log = std::getenv("SWORDFS_E2E_LOG");
  return log ? log : work_dir_ + "/swordfs.log";
}

void Fixture::InitPaths() {
  // Use the fully-qualified gtest name (suite_test) as the unique
  // namespace so that parallel / repeated runs never collide.
  auto *test_info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string test_name = std::string(test_info->test_suite_name()) + "_" + test_info->name();

  volume_name_ = test_name;
  work_dir_ = "/tmp/swordfs_e2e_" + volume_name_;
  mountpoint_ = work_dir_ + "/mnt";
  vol_config_dir_ = work_dir_ + "/config";

  // Restore base URL and append test-specific prefix.
  bucket_url_ = base_bucket_url_ + volume_name_;
}

}  // namespace e2e
}  // namespace swordfs
