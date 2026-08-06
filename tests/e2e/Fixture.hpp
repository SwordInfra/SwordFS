// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Fixture — end-to-end test harness for SwordFS.
//
// Each test case:
//   1. Creates a temp directory and unique S3 prefix
//   2. Runs `swordfs format` via C++ API
//   3. Runs `swordfs mount -f` as a subprocess
//   4. Waits for the FUSE mount to become ready
//   5. Provides helper methods for POSIX filesystem operations
//   6. On teardown: unmounts, kills the daemon, cleans up

#pragma once

#include <sys/stat.h>
#include <sys/statvfs.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace swordfs {
namespace e2e {

/// RAII harness that manages a mounted SwordFS volume for one test case.
class Fixture {
 public:
  Fixture();
  ~Fixture();

  // ── Lifecycle (called by SetUp/TearDown) ──────────────────────

  /// Format the volume and mount it.  Returns true on success.
  bool SetUp();

  /// Unmount and clean up.
  void TearDown();

  // ── Filesystem helpers ────────────────────────────────────────

  /// statvfs() on the mountpoint.
  struct statvfs Statfs();

  /// Read all contents of a file into |out|.
  /// Returns 0 on success, or a positive errno value on failure.
  int ReadFile(const std::string& relpath, std::string* out);

  /// Compare file content with expected via hash.  On mismatch, prints
  /// only sizes and hashes — never dumps raw binary content.
  ::testing::AssertionResult CheckFile(const std::string& relpath,
                                        const std::string& expected);

  /// Return true if file content equals expected (no GTest output).
  /// Suitable for use inside loops / threads where assertion macros
  /// cannot be used.
  bool FileEquals(const std::string& relpath,
                  const std::string& expected);

  /// Write data to a file (creates if not exists, truncates if exists).
  int WriteFile(const std::string& relpath, const std::string& data);

  /// List directory entries (excluding . and ..).
  std::vector<std::string> ReadDir(const std::string& relpath);

  /// Return the absolute path for a relative path under the mountpoint.
  std::string MountPath(const std::string& relpath) const;

  /// Return true if the mount is currently active.
  bool IsMounted() const;

  // ── Accessors ─────────────────────────────────────────────────

  const std::string& work_dir() const { return work_dir_; }
  const std::string& mountpoint() const { return mountpoint_; }
  const std::string& bucket_url() const { return bucket_url_; }

 private:
  bool FormatVolume();
  bool StartMount();
  bool WaitForMount();
  bool StopMount();
  std::string FindSwordfsBin() const;

  std::string work_dir_;
  std::string mountpoint_;
  std::string vol_config_dir_;
  std::string volume_name_;
  std::string bucket_url_;
  std::string base_bucket_url_;  // original URL (without test-name suffix)
  bool mounted_ = false;
};

}  // namespace e2e
}  // namespace swordfs
