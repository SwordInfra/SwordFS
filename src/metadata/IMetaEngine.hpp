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

/// Well-known metadata engine URLs.
constexpr std::string_view kMemoryMetaUrl = "memory://local";

/// Returns true if |meta_url| refers to the in-memory metadata engine.
inline bool IsMemoryMode(std::string_view meta_url) {
  return meta_url == kMemoryMetaUrl;
}

/// Concurrency contract: every method on this interface must be atomic
/// and thread-safe.  Concurrent observers must never see an intermediate
/// state of a composite operation (e.g. a Rename whose target has been
/// unlinked but whose source has not yet been moved).  For KV-backed
/// implementations each method is expected to map onto a single
/// transaction.
class IMetaEngine {
 public:
  virtual ~IMetaEngine() = default;

  /// Return filesystem limits provided by this metadata engine.
  virtual Limits GetLimits() const = 0;

  /// Look up a child entry by name.
  virtual Status Lookup(InodeID parent_ino, std::string_view name,
                        SwordFsInode *out) = 0;

  /// Get an inode metadata snapshot.
  virtual Status GetInode(InodeID ino, SwordFsInode *out) = 0;

  /// List all entries in a directory.
  virtual Status ReadDir(InodeID ino, std::vector<SwordFsEntry> *entries) = 0;

  /// Create a regular file.
  virtual Status Create(InodeID parent_ino, std::string_view name,
                        mode_t mode, SwordFsInode *out) = 0;

  /// Create a directory. Increments parent nlink to account for "..".
  virtual Status MkDir(InodeID parent_ino, std::string_view name,
                       mode_t mode, SwordFsInode *out) = 0;

  /// POSIX unlink(2): detach the directory entry and decrement nlink.
  /// On success, *post_nlink receives the authoritative nlink value the
  /// caller needs to decide what to do next:
  ///   - For a directory: the entry is removed and the inode is dropped
  ///     atomically; *post_nlink is set to 0 (the inode no longer exists).
  ///   - For a file: *post_nlink is the post-decrement nlink (which may
  ///     still be >0 if another hardlink name exists, or ==0 if this
  ///     was the last name).
  ///
  /// This avoids the TOCTOU race of reading nlink before the unlink and
  /// deciding afterwards: the store is the only thing that mutates
  /// nlink on this thread, and we read it back under the same lock.
  virtual Status Unlink(InodeID parent_ino, std::string_view name,
                        nlink_t *post_nlink = nullptr) = 0;

  /// Remove an empty directory. Decrements parent nlink.
  virtual Status RmDir(InodeID parent_ino, std::string_view name) = 0;

  /// Rename (move) an entry between directories.  |flags| is a bitwise
  /// OR of RenameFlag values.  When |result| is non-null and the rename
  /// replaced an existing non-directory entry, the implementation fills
  /// |*result| as part of the same atomic mutation so the caller can
  /// reclaim the overwritten inode's data without a second lookup.
  /// Engines that cannot report the overwritten inode may leave
  /// |*result| untouched.
  virtual Status Rename(InodeID old_parent_ino,
                        std::string_view old_name, InodeID new_parent_ino,
                        std::string_view new_name, RenameFlag flags,
                        RenameResult *result = nullptr) = 0;

  /// Set attributes for an inode.  |fields| is a bitwise OR of
  /// SetAttrField values; only the bits set in |fields| are read from
  /// |attr| and applied to the inode.
  virtual Status SetAttr(InodeID ino, const struct stat *attr,
                         SetAttrField fields, SwordFsInode *out) = 0;

  /// Get file system statistics.
  virtual Status StatFs(struct statvfs *stbuf) = 0;

  /// Check access permissions.
  virtual Status Access(InodeID ino, int mask) = 0;

  /// Create a symbolic link.
  virtual Status Symlink(InodeID parent_ino, std::string_view name,
                         const char *link, SwordFsInode *out) = 0;

  /// Create a hard link to an existing inode.
  virtual Status Link(InodeID ino, InodeID newparent_ino,
                      std::string_view newname, SwordFsInode *out) = 0;

  /// Read the target of a symbolic link.
  virtual Status Readlink(InodeID ino, std::string *target) = 0;

  /// Open a regular file.  Performs the permission check (regular-file
  /// validation + read permission) and updates atime.
  virtual Status Open(InodeID ino) = 0;

  /// Delete an inode that was previously unlinked. Called by the VFS layer
  /// once it has verified no open file descriptor still references the
  /// inode (e.g. from the last `Close` after an open-unlink). The metadata
  /// engine removes the inode and its chunk-metadata map when nlink has
  /// dropped to zero; otherwise this is a no-op.
  ///
  /// @important  This does NOT delete the chunk objects from the data
  /// engine. The caller is responsible for invoking
  /// `IDataEngine::Delete` on each chunk key first (use `ListChunks` to
  /// enumerate). See `vfs::InodeHandle::ReclaimData` for the canonical
  /// implementation of the full cleanup.
  virtual Status ReclaimInode(InodeID ino) = 0;

  /// Enumerate the chunk indices currently registered for |ino|. Fills
  /// |*out| with `ChunkMeta` entries (by value) ordered by ascending
  /// chunk index. Used by the VFS layer when reclaiming an inode to
  /// compute the per-chunk object keys it must delete via the data
  /// engine. Returns OK with `*out` empty if the inode has no chunks.
  virtual Status ListChunks(InodeID ino, std::vector<ChunkMeta> *out) = 0;

  /// Open a directory for reading. Performs permission check and updates
  /// atime. Handle allocation is now managed by FileHandleManager.
  virtual Status OpenDir(InodeID ino) = 0;

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
