// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "tests/e2e/Fixture.hpp"

#include <folly/FileUtil.h>
#include <folly/portability/Filesystem.h>
#include <folly/hash/Hash.h>

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
  // Save the base URL so SetUp() can be called multiple times
  // without accumulating test-name suffixes.
  base_bucket_url_ = bucket_url_;
}

Fixture::~Fixture() {
  TearDown();
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

  // Restore base URL and append test-specific prefix.
  bucket_url_ = base_bucket_url_ + test_name;

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
  // Idempotent: skip if already torn down.
  if (mountpoint_.empty()) return;

  StopMount();
  mounted_ = false;

  if (!std::getenv("SWORDFS_E2E_KEEP_WORKDIR")) {
    std::string cmd = "rm -rf " + work_dir_;
    std::system(cmd.c_str());
  }

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
  const char* log_override = std::getenv("SWORDFS_E2E_LOG");
  std::string log_path = log_override ? log_override
                                      : work_dir_ + "/swordfs.log";
  cmd << FindSwordfsBin()
      << " --log-file " << log_path
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
    // Verify the mountpoint is actually a FUSE filesystem, not just
    // a regular directory on the host.
    struct statvfs sv;
    if (::statvfs(mountpoint_.c_str(), &sv) == 0 &&
        sv.f_type == 0x65735546 /* FUSE_SUPER_MAGIC */) {
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
  std::string cmd = "fusermount3 -u " + mountpoint_;
  if (std::system(cmd.c_str()) != 0) {
    // Force-unmount as fallback.
    cmd = "fusermount3 -uz " + mountpoint_;
    std::system(cmd.c_str());
  }
  return true;
}

// ────────────────────────────────────────────────────────────────
// Filesystem helpers
// ────────────────────────────────────────────────────────────────

std::string Fixture::MountPath(const std::string& relpath) const {
  if (relpath.empty() || relpath == ".") return mountpoint_;
  return mountpoint_ + "/" + relpath;
}

bool Fixture::IsMounted() const {
  if (!mounted_) return false;
  struct statvfs sv;
  return ::statvfs(mountpoint_.c_str(), &sv) == 0;
}

struct statvfs Fixture::Statfs() {
  struct statvfs sv {};
  if (::statvfs(mountpoint_.c_str(), &sv) != 0 ||
      sv.f_type != 0x65735546 /* FUSE_SUPER_MAGIC */) {
    std::memset(&sv, 0, sizeof(sv));
  }
  return sv;
}

int Fixture::ReadFile(const std::string& relpath, std::string* out) {
  if (!folly::readFile(MountPath(relpath).c_str(), *out)) {
    return errno;
  }
  return 0;
}

namespace {
std::string Hash64Hex(std::string_view data) {
  auto h = folly::hash::SpookyHashV2::Hash64(data.data(), data.size(), 0);
  char buf[18];  // "0x" + 16 hex digits + NUL
  std::snprintf(buf, sizeof(buf), "0x%016lx", h);
  return buf;
}
}  // namespace

::testing::AssertionResult Fixture::CheckFile(
    const std::string& relpath, const std::string& expected) {
  std::string actual;
  int err = ReadFile(relpath, &actual);
  if (err != 0) {
    return ::testing::AssertionFailure()
        << "Failed to read \"" << relpath << "\": "
        << std::strerror(err) << " (errno=" << err << ")";
  }
  if (actual == expected) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
      << "File content mismatch for \"" << relpath << "\""
      << "\n  Expected: size=" << expected.size()
      << " hash=" << Hash64Hex(expected)
      << "\n  Actual:   size=" << actual.size()
      << " hash=" << Hash64Hex(actual);
}

bool Fixture::FileEquals(const std::string& relpath,
                         const std::string& expected) {
  std::string data;
  return ReadFile(relpath, &data) == 0 && data == expected;
}

int Fixture::WriteFile(const std::string& relpath,
                           const std::string& data) {
  // Pin umask to 022 so newly-created files get 0644 regardless of
  // the caller's environment.
  mode_t old_umask = ::umask(022);
  if (!folly::writeFile(data, MountPath(relpath).c_str())) {
    ::umask(old_umask);
    return errno;
  }
  ::umask(old_umask);
  return 0;
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
