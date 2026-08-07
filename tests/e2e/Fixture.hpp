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

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

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
  /// Returns 0 on success, or -1 on failure (errno is set).
  int ReadFile(const std::string &relpath, std::string *out);

  /// Write data to a file (creates if not exists, truncates if exists).
  /// Returns 0 on success, or -1 on failure (errno is set).
  int WriteFile(const std::string &relpath, const std::string &data);

  /// Compare file content with expected via 64-bit hash.
  /// On mismatch, prints sizes and hashes — never dumps raw content.
  /// Returns an AssertionResult (usable with EXPECT_TRUE) that also
  /// converts to bool for use in loops / threads.
  ::testing::AssertionResult FileEquals(const std::string &relpath,
                                        size_t expected_size,
                                        uint64_t expected_hash);

  /// Compute a 64-bit SpookyHashV2 for use with FileEquals.
  static uint64_t Hash64(std::string_view data);

  /// List directory entries (excluding . and ..).
  /// Returns 0 on success, or -1 on failure (errno is set).
  int ReadDir(const std::string &relpath, std::vector<std::string> *entries);

  /// Return the absolute path for a relative path under the mountpoint.
  std::string MountPath(const std::string &relpath) const;

  /// Return true if the mount is currently active.
  bool IsMounted();

 private:
  bool FormatVolume();
  bool StartMount();
  bool WaitForMount();
  bool StopMount();
  void InitPaths();
  std::string LogPath() const;
  std::string FindSwordfsBin() const;

 private:
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
