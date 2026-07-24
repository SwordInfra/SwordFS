// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS metadata — abstract interface for inode and directory
// operations. First implementation is in-memory (MemMetaImpl); a TiKV-backed
// implementation will follow.

#pragma once

#include <sys/stat.h>
#include <sys/statvfs.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "metadata/Types.hpp"
#include "storage/Slice.hpp"
#include "utils/Context.hpp"
#include "utils/Status.hpp"

using Status = swordfs::utils::Status;
using SwordFsContext = swordfs::utils::SwordFsContext;

namespace swordfs::metadata {

class Meta {
 public:
  virtual ~Meta() = default;

  /// Look up a child entry by name.
  virtual Status Lookup(InodeID parent_ino,
                        std::string_view name, InodeID* child_ino,
                        struct stat* attr) = 0;

  /// Get attributes for an inode.
  virtual Status GetAttr(InodeID ino, struct stat* attr) = 0;

  /// List all entries in a directory.
  virtual Status ReadDir(InodeID ino, std::vector<SwordFsEntry>* entries) = 0;

  /// Create a regular file.
  virtual Status Create(InodeID parent_ino,
                        std::string_view name, mode_t mode,
                        InodeID* child_ino, struct stat* attr) = 0;

  /// Create a directory. Increments parent nlink to account for "..".
  virtual Status MkDir(InodeID parent,
                       std::string_view name, mode_t mode,
                       InodeID* child_ino, struct stat* attr) = 0;

  /// Remove a regular file.
  virtual Status Unlink(InodeID parent_ino, std::string_view name) = 0;

  /// Remove an empty directory. Decrements parent nlink.
  virtual Status RmDir(InodeID parent_ino, std::string_view name) = 0;

  /// Rename (move) an entry between directories.
  ///
  /// flags may be 0 (normal POSIX rename), RENAME_NOREPLACE (fail if
  /// target exists), or RENAME_EXCHANGE (atomically swap src and dst).
  virtual Status Rename(InodeID old_parent_ino,
                        std::string_view old_name, InodeID new_parent_ino,
                        std::string_view new_name, unsigned int flags) = 0;

  /// Set attributes for an inode.
  virtual Status SetAttr(InodeID ino,
                         const struct stat* attr, int to_set,
                         struct stat* out_attr) = 0;

  /// Get file system statistics.
  virtual Status StatFs(struct statvfs* stbuf) = 0;

  /// Check access permissions.
  virtual Status Access(InodeID ino, int mask) = 0;

  /// Open a regular file. Allocates a handle (*fh) that is passed to
  /// subsequent read/write/flush calls.
  virtual Status Open(InodeID ino, uint64_t* fh) = 0;

  /// Release a file handle. Called when the last reference to this open
  /// instance is closed.
  virtual Status Release(uint64_t fh) = 0;

  /// Open a directory for reading. Allocates a handle (*fh) that is passed
  /// to subsequent readdir calls.
  virtual Status OpenDir(InodeID ino, uint64_t* fh) = 0;

  /// Release a directory handle.
  virtual Status ReleaseDir(uint64_t fh) = 0;

  /// Decrement the inode's lookup count by nlookup. Called in response to
  /// FUSE forget requests. The caller may free or reuse the inode's backend
  /// resources when the count reaches zero.
  virtual Status Forget(InodeID ino, uint64_t nlookup) = 0;

  // ── Chunk Slice operations (S3 Phase 2) ──────────────────────────

  /// Append a slice to an inode's chunk list.
  virtual Status AppendSlice(InodeID ino,
                             const storage::Slice& slice) = 0;

  /// Retrieve the full slice list for an inode.
  virtual Status GetSlices(InodeID ino,
                           storage::SliceList* out) = 0;

  /// Return the next slice id for an inode (atomically increments).
  virtual uint64_t NextSliceID(InodeID ino) = 0;
};

}  // namespace swordfs::metadata
