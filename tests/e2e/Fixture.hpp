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

  /// Create a directory relative to the mountpoint.
  bool Mkdir(const std::string& relpath, mode_t mode = 0755);

  /// Remove an empty directory.
  bool Rmdir(const std::string& relpath);

  /// Remove a file.
  bool Unlink(const std::string& relpath);

  /// Rename a file or directory.
  bool Rename(const std::string& oldpath, const std::string& newpath);

  /// stat() a path relative to the mountpoint.
  struct stat Stat(const std::string& relpath);

  /// statvfs() on the mountpoint (or a subdirectory).
  struct statvfs Statfs(const std::string& relpath = "");

  /// Check access permissions.
  int Access(const std::string& relpath, int mode);

  /// Read all contents of a file into a string.
  std::string ReadFile(const std::string& relpath);

  /// Write data to a file (creates if not exists, truncates if exists).
  bool WriteFile(const std::string& relpath, const std::string& data);

  /// List directory entries (excluding . and ..).
  std::vector<std::string> ReadDir(const std::string& relpath = "");

  /// Truncate a file to the given size.
  bool Truncate(const std::string& relpath, off_t size);

  /// Chmod a path.
  bool Chmod(const std::string& relpath, mode_t mode);

  /// Create a hard link (currently not supported by SwordFS).
  bool Link(const std::string& target, const std::string& linkpath);

  /// Create a symbolic link (currently not supported by SwordFS).
  bool Symlink(const std::string& target, const std::string& linkpath);

  /// Return the absolute path for a relative path under the mountpoint.
  std::string MountPath(const std::string& relpath = "") const;

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
  bool mounted_ = false;
};

}  // namespace e2e
}  // namespace swordfs
