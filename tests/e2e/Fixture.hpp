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

#include "metadata/IMetaEngine.hpp"

namespace swordfs {
namespace e2e {

/// Lightweight descriptor for a directory entry used in test setup.
struct Child {
  std::string name;
  std::string content;  // empty for directories
  bool is_dir = false;
};

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

  // ── POSIX wrappers ───────────────────────────────────────────

  /// stat() on a file or directory under the mountpoint.
  /// Returns 0 on success, or -1 on failure (errno is set).
  int Stat(const std::string &relpath, struct stat *st) const;

  /// lstat() — same as stat() but does not follow symlinks.
  int Lstat(const std::string &relpath, struct stat *st) const;

  /// statvfs() on the mountpoint.  Returns 0 on success, or -1.
  int Statfs(struct statvfs *sv) const;

  /// Check file accessibility.  Returns 0 on success, or -1.
  int Access(const std::string &relpath, int mode);

  /// Create a regular file.  flags are passed to ::open (e.g. O_EXCL).
  /// Returns 0 on success, or -1 on failure (errno is set).
  int CreateFile(const std::string &relpath, mode_t mode, int flags);

  /// Open a file.  Returns fd on success, or -1 on failure (errno is set).
  int OpenFile(const std::string &relpath, int flags);

  /// Read all contents of a file into |out|.
  /// Returns 0 on success, or -1 on failure (errno is set).
  int ReadFile(const std::string &relpath, std::string *out);

  /// Write data to a file (file must already exist).
  /// Returns 0 on success, or -1 on failure (errno is set).
  int WriteFile(const std::string &relpath, const std::string &data);

  /// List directory entries (excluding . and ..).
  /// Returns 0 on success, or -1 on failure (errno is set).
  int ReadDir(const std::string &relpath, std::vector<std::string> *entries);

  /// Unlink (delete) a file.  Returns 0 on success, or -1.
  int UnlinkFile(const std::string &relpath);

  /// Create a directory.  Returns 0 on success, or -1.
  int MkDir(const std::string &relpath, mode_t mode);

  /// Remove an empty directory.  Returns 0 on success, or -1.
  int RmDir(const std::string &relpath);

  /// Rename (move) a file or directory.  Returns 0 on success, or -1.
  int Rename(const std::string &oldpath, const std::string &newpath);

  /// Create a symbolic link. Returns 0 on success, or -1.
  int Symlink(const std::string &target, const std::string &linkpath);

  /// Create a hard link. Returns 0 on success, or -1.
  int HardLink(const std::string &oldpath, const std::string &newpath);

  /// Read a symbolic link target. Returns 0 on success, or -1.
  int Readlink(const std::string &relpath, std::string *target);

  /// Change file mode.  Returns 0 on success, or -1.
  int Chmod(const std::string &relpath, mode_t mode);

  /// Truncate a file to a given size.  Returns 0 on success, or -1.
  int Truncate(const std::string &relpath, off_t length);

  // ── Test utilities ────────────────────────────────────────────

  /// Return true if the mount is currently active.
  bool IsMounted();

  /// Return true if the daemon process recorded during SetUp is gone.
  bool IsDaemonGone() const;

  /// Return the absolute path for a relative path under the mountpoint.
  std::string MountPath(const std::string &relpath) const;

  /// Check that the file permissions match |requested_mode| minus umask.
  ::testing::AssertionResult UmaskEquals(const std::string &relpath, mode_t expected_mask) const;

  /// Compare file content with expected via 64-bit hash.
  /// On mismatch, prints sizes and hashes — never dumps raw content.
  ::testing::AssertionResult FileEquals(const std::string &relpath, size_t expected_size, uint64_t expected_hash);

  /// Compute a 64-bit SpookyHashV2 for use with FileEquals.
  static uint64_t Hash64(std::string_view data);

  /// Controls the length of data produced by GenerateRandomData().
  enum class RandomMode {
    kExact,  ///< Exactly |len| bytes.
    kUpTo,   ///< Random length in [1, len].
  };

  /// Generate pseudo-random data.
  /// - RandomMode::kExact: returns exactly |len| bytes.
  /// - RandomMode::kUpTo:  returns a random length in [1, len].
  static std::string GenerateRandomData(size_t len, RandomMode mode);

  /// Return the filesystem limits for this test run.
  metadata::Limits GetLimits() const;

 private:
  bool FormatVolume();
  bool StartMount();
  bool WaitForMount();
  bool StopMount();
  void InitPaths();
  void RemoveVolumeConfig();
  std::string LogPath() const;
  std::string FindSwordfsBin() const;

 private:
  std::string work_dir_;
  std::string mountpoint_;
  std::string volume_name_;
  // Keep the formatted Redis volume across unmount/remount cycles within a test.
  bool volume_formatted_ = false;
  std::string bucket_url_;
  std::string base_bucket_url_;  // original URL (without test-name suffix)
  bool mounted_ = false;
  pid_t daemon_pid_ = 0;
};

}  // namespace e2e
}  // namespace swordfs
