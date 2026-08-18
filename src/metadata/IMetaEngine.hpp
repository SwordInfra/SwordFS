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
#include "utils/Context.hpp"
#include "utils/Status.hpp"

using Status = swordfs::utils::Status;
using SwordFsContext = swordfs::utils::SwordFsContext;

namespace swordfs::metadata {

/// Filesystem limits provided by each metadata engine.
struct Limits {
  /// Maximum length of a single path component (POSIX NAME_MAX).
  size_t max_name_length;
  /// Maximum free inodes, reported as f_ffree in statvfs.
  size_t max_free_inodes;
};

/// Well-known metadata engine URLs.
constexpr std::string_view kMemoryMetaUrl = "memory://local";

/// Returns true if |meta_url| refers to the in-memory metadata engine.
inline bool IsMemoryMode(std::string_view meta_url) {
  return meta_url == kMemoryMetaUrl;
}

class IMetaEngine {
 public:
  virtual ~IMetaEngine() = default;

  /// Return limits for the given metadata engine type.
  static Limits GetLimits(std::string_view meta_url);

  /// Look up a child entry by name.
  virtual Status Lookup(InodeID parent_ino,
                        std::string_view name, InodeID *child_ino,
                        struct stat *attr) = 0;

  /// Get attributes for an inode.
  virtual Status GetAttr(InodeID ino, struct stat *attr) = 0;

  /// List all entries in a directory.
  virtual Status ReadDir(InodeID ino, std::vector<SwordFsEntry> *entries) = 0;

  /// Create a regular file.
  virtual Status Create(InodeID parent_ino,
                        std::string_view name, mode_t mode,
                        InodeID *child_ino, struct stat *attr) = 0;

  /// Create a directory. Increments parent nlink to account for "..".
  virtual Status MkDir(InodeID parent,
                       std::string_view name, mode_t mode,
                       InodeID *child_ino, struct stat *attr) = 0;

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
                         const struct stat *attr, int to_set,
                         struct stat *out_attr) = 0;

  /// Get file system statistics.
  virtual Status StatFs(struct statvfs *stbuf) = 0;

  /// Check access permissions.
  virtual Status Access(InodeID ino, int mask) = 0;

  /// Create a symbolic link.
  virtual Status Symlink(InodeID parent_ino,
                         std::string_view name, const char *link,
                         InodeID *child_ino, struct stat *attr) = 0;

  /// Create a hard link to an existing inode.
  virtual Status Link(InodeID ino, InodeID newparent_ino,
                      std::string_view newname, struct stat *attr) = 0;

  /// Read the target of a symbolic link.
  virtual Status Readlink(InodeID ino, std::string *target) = 0;

  /// Open a regular file.  Performs the permission check (regular-file
  /// validation + read permission) and updates atime.
  virtual Status Open(InodeID ino) = 0;

  /// Reclaim the data of an inode that was unlinked while still open.
  /// The VFS layer calls this once no open file descriptors remain.  The
  /// metadata engine deletes the inode and its chunks only if the inode is
  /// orphaned (nlink==0); otherwise this is a no-op.
  virtual Status ReclaimData(InodeID ino) = 0;

  /// Open a directory for reading. Performs permission check and updates
  /// atime. Handle allocation is now managed by FileHandleManager.
  virtual Status OpenDir(InodeID ino) = 0;

  /// Decrement the inode's lookup count by nlookup. Called in response to
  /// FUSE forget requests. The caller may free or reuse the inode's backend
  /// resources when the count reaches zero.
  virtual Status Forget(InodeID ino, uint64_t nlookup) = 0;

  // ────────────────────────────────────────────────────────────────
  // Chunk metadata
  // ────────────────────────────────────────────────────────────────

  /// Register a flushed chunk.  The metadata engine stores the chunk
  /// key, size, and start offset so that subsequent reads can locate
  /// the data via the data engine.
  virtual Status AddChunk(InodeID ino, const ChunkMeta &cm) = 0;

  /// Find the chunk at |idx|.  Returns OK and fills |*cm| if a
  /// matching chunk is registered for the given inode.
  virtual Status FindChunk(InodeID ino, ChunkIndex idx, ChunkMeta *cm) = 0;

  /// Truncate |ino| to |size| bytes.  Updates the inode size and drops
  /// chunk metadata for data beyond the new size.
  virtual Status Truncate(InodeID ino, size_t size) = 0;
};

}  // namespace swordfs::metadata
