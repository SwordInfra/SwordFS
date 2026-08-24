// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Core POD types for the metadata subsystem.
//
// This header does NOT pull in Folly (F14Map). If you need F14FastMap,
// include <folly/container/F14Map.h> directly.

#pragma once

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdint>
#include <ctime>
#include <string>

namespace swordfs::metadata {

// forward declaration — most users only hold pointers
struct SwordFsInode;

// ────────────────────────────────────────────────────────────────
// 1. Type aliases and constants
// ────────────────────────────────────────────────────────────────

using InodeID = uint64_t;
using ChunkIndex = uint32_t;  // 0-based chunk number within a file

/// Reserved inode id for the filesystem root. Value matches
/// `FUSE_ROOT_ID` so VFS/FUSE code that already assumes `1` keeps
/// working without translation; metadata code should refer to this
/// constant (not the FUSE macro) so it stays ABI-neutral.
constexpr InodeID kRootInodeId = 1;

// ────────────────────────────────────────────────────────────────
// 2. Plain value types
// ────────────────────────────────────────────────────────────────

/// One directory entry returned by IMetaEngine::ReadDir.  Carries the
/// target's ino, its DT_* type, and the directory it lives in
/// (parent_ino).  The synthetic "." and ".." entries are produced by
/// the backend inside ReadDir and carry the same shape.
struct SwordFsEntry {
  std::string name;
  uint32_t type;  // DT_DIR, DT_REG, DT_LNK, etc.
  InodeID ino;
};

/// Filesystem limits provided by each metadata engine.
struct Limits {
  /// Maximum length of a single path component (POSIX NAME_MAX).
  size_t max_name_length;
  /// Maximum free inodes, reported as f_ffree in statvfs.
  size_t max_free_inodes;
};

/// Metadata for one flushed chunk.
struct ChunkMeta {
  ChunkIndex index;       // 0-based chunk number
  uint64_t start_offset;  // file offset of the chunk's first byte
  std::string key;        // storage key (e.g. "42/0")
  size_t size;            // data size in bytes
};

// ────────────────────────────────────────────────────────────────
// 3. SetAttr / Rename bitflags
// ────────────────────────────────────────────────────────────────
// Bit values deliberately match the kernel constants (FUSE_SET_ATTR_*
// and RENAME_*) so the fuse-layer translator does a 1:1 copy.  Helpers
// below operate on the same bit values via uint32_t views.

// Bitfield of attributes to update on a SetAttr call.  Bit values are
// deliberately non-contiguous (matching the FUSE_SET_ATTR_* constants
// in <fuse_lowlevel.h>) so the fuse-layer translator can do a 1:1 copy
// without losing precision.
enum class SetAttrField : uint32_t {
  kMode = 1u << 0,
  kUid = 1u << 1,
  kGid = 1u << 2,
  kSize = 1u << 3,
  kAtime = 1u << 4,
  kMtime = 1u << 5,
  kAtimeNow = 1u << 7,  // bit 6 reserved
  kMtimeNow = 1u << 8,
  kCtime = 1u << 10,  // bits 9 reserved
};

// Flags accepted by Rename.  Bit values match the kernel RENAME_*
// constants so the fuse-layer translator maps 1:1.
enum class RenameFlag : uint32_t {
  kNone = 0,
  kNoReplace = 1u << 0,
  kExchange = 1u << 1,
};

// Result of a rename that replaced an existing non-directory entry.
// The metadata transaction owns the atomic mutation; the VFS layer uses
// this result to decide whether the overwritten inode's data needs to be
// reclaimed without performing a second lookup.
struct RenameResult {
  InodeID overwritten_ino = 0;
  nlink_t overwritten_post_nlink = 0;
};

inline bool HasSetAttrField(SetAttrField fields, SetAttrField field) {
  return (static_cast<uint32_t>(fields) & static_cast<uint32_t>(field)) != 0;
}

inline SetAttrField operator|(SetAttrField a, SetAttrField b) {
  return static_cast<SetAttrField>(static_cast<uint32_t>(a) |
                                   static_cast<uint32_t>(b));
}

inline SetAttrField &operator|=(SetAttrField &a, SetAttrField b) {
  a = a | b;
  return a;
}

inline bool HasRenameFlag(RenameFlag flags, RenameFlag flag) {
  return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}

inline RenameFlag operator|(RenameFlag a, RenameFlag b) {
  return static_cast<RenameFlag>(static_cast<uint32_t>(a) |
                                 static_cast<uint32_t>(b));
}

inline RenameFlag &operator|=(RenameFlag &a, RenameFlag b) {
  a = a | b;
  return a;
}

// ────────────────────────────────────────────────────────────────
// 4. SwordFsInode
// ────────────────────────────────────────────────────────────────
// The core per-inode record.  Methods are declared here and defined
// in Types.cpp so the header stays free of inline definitions.

struct SwordFsInode {
  InodeID ino = 0;
  struct stat attr {};
  InodeID parent_ino = 0;        // Directory the inode lives in.
  std::string symlink_target;    // non-empty only for S_IFLNK

  SwordFsInode() = default;
  SwordFsInode(InodeID ino, struct stat attr, InodeID parent_ino,
               std::string symlink_target = std::string{});

  // Bump one or more inode timestamps to now.
  void Touch(SetAttrField fields);

  bool IsDir() const;
  bool IsRegular() const;
  bool IsSymlink() const;

  // POSIX access check. Returns true if uid/gid has the requested permissions
  // on this inode. Root (uid == 0) always has full access.
  bool CheckAccess(uid_t uid, gid_t gid, int mask) const;

  // Sticky-bit (S_ISVTX) deletion check, called on the containing
  // directory: returns true if |uid| may unlink/rename |target| within
  // it — i.e. the bit is clear, or uid is root, the directory owner, or
  // the entry's owner.
  bool CheckStickyDelete(uid_t uid, const SwordFsInode &target) const;
};

}  // namespace swordfs::metadata