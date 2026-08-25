// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/mem/MemMetaImpl.hpp"

#include <dirent.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <folly/logging/xlog.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "metadata/MetaEngineRegistry.hpp"
#include "metadata/Types.hpp"
#include "metadata/Utils.hpp"
#include "utils/Logging.hpp"

namespace swordfs::metadata {

namespace {
utils::Status CreateMemoryMetaEngine(std::string_view, std::string_view, std::unique_ptr<IMetaEngine> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("metadata engine output is null");
  }
  *out = std::make_unique<MemMetaImpl>();
  return utils::Status::OK();
}

// MemMeta-specific filesystem limits.
constexpr size_t kMaxNameLength = 255;  // POSIX NAME_MAX
constexpr size_t kMaxFreeInodes = UINT64_MAX;

RegisterMetaEngine kMemoryMetaEngine{"memory", CreateMemoryMetaEngine};

}  // namespace

// Transaction model: every method below runs its metadata mutation as a
// single store_.Transact() script, so each IMetaEngine operation is atomic.
//
// Discipline: a Transact() script contains MemMetaTxn semantic operations
// plus pure policy decisions on value snapshots. Logging and ambient
// context are captured outside the transaction. MemMetaTxn owns metadata
// state transitions that must remain atomic; this layer should not replay
// their bookkeeping by hand.
//
// Division of labour: the primitives maintain the tree's structural
// invariants (parent nlink on directory link/unlink/move, mtime/ctime
// on entry-list changes, ctime on re-linked inodes) — this layer only
// contributes POLICY: name validation, permission and sticky checks,
// cycle prevention, rename flags, and POSIX error codes.
//
// Lookup idiom: check the Lookup status first and return it unchanged
// (a missing inode is NotFound, NOT NotDirectory), then test the type
// predicate on the snapshot:
//
//   SwordFsInode parent;
//   Status status = txn.LookupInode(parent_ino, &parent);
//   if (!status.ok()) {
//     return status;
//   }
//   if (!parent.IsDir()) {
//     return Status::NotDirectory("parent is not a directory");
//   }

// ────────────────────────────────────────────────────────────────
// Entry operations
// ────────────────────────────────────────────────────────────────

Status MemMetaImpl::Lookup(InodeID parent_ino, std::string_view name, SwordFsInode *out) {
  SwordFsInode child;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    Status status = txn.LookupEntry(parent_ino, name, &child);
    if (!status.ok()) {
      return status;
    }
    return Status::OK();
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Lookup: parent=" << parent_ino << " name='" << name << "' failed: " << status.message();
    return status;
  }
  SWORDFS_LOG_DEBUG << "Lookup: parent=" << parent_ino << " name='" << name << "' -> ino=" << child.ino;

  if (out) {
    *out = child;
  }
  return Status::OK();
}

Status MemMetaImpl::GetInode(InodeID ino, SwordFsInode *out) {
  SwordFsInode inode;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status { return txn.LookupInode(ino, &inode); });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "GetInode: ino " << ino << " failed: " << status.message();
    return status;
  }
  if (out) {
    *out = inode;
  }
  return Status::OK();
}

Status MemMetaImpl::Create(InodeID parent_ino, std::string_view name, mode_t mode, SwordFsInode *out) {
  if (name.size() > kMaxNameLength) {
    return Status::NameTooLong("file name exceeds maximum length");
  }

  const SwordFsContext ctx = folly::fibers::local<SwordFsContext>();
  SwordFsInode child;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode parent;
    Status status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    // Check permissions: need write+execute on the parent directory
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }

    mode_t file_mode = (S_IFREG | (mode & 0777));
    return txn.AddEntry(parent_ino, name, file_mode, &child);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Create: parent=" << parent_ino << " name='" << name << "' failed: " << status.message();
    return status;
  }

  SWORDFS_LOG_DEBUG << "Create: parent=" << parent_ino << " name='" << name << "' -> ino=" << child.ino;
  if (out) {
    *out = child;
  }
  return Status::OK();
}

Status MemMetaImpl::Unlink(InodeID parent_ino, std::string_view name, nlink_t *post_nlink) {
  // Refuse to unlink "." or ".."
  if (name == "." || name == "..") {
    return Status::InvalidArgument("cannot unlink . or ..");
  }

  const SwordFsContext ctx = folly::fibers::local<SwordFsContext>();
  InodeID target_ino = 0;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode parent;
    Status status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }

    // Permission check on parent directory
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }

    SwordFsInode target;
    status = txn.LookupEntry(parent_ino, name, &target);
    if (!status.ok()) {
      return status;
    }

    // Sticky bit on the parent directory: only root, the directory
    // owner, or the entry's owner can unlink entries.
    if (!parent.CheckStickyDelete(ctx.uid, target)) {
      return Status::Permission("sticky bit denied");
    }

    if (target.IsDir()) {
      return Status::InvalidArgument("cannot unlink directory");
    }

    target_ino = target.ino;

    // Unlink only detaches the directory entry and decrements nlink; the
    // transaction hands back the authoritative post-decrement nlink in
    // *post_nlink so the caller doesn't have to re-read it (avoiding the
    // TOCTOU race that an unlink-before-read decision would have).
    return txn.Unlink(parent_ino, name, post_nlink);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Unlink: parent=" << parent_ino << " name='" << name << "' failed: " << status.message();
    return status;
  }
  SWORDFS_LOG_DEBUG << "Unlink: parent=" << parent_ino << " name='" << name << "' ino=" << target_ino;
  return Status::OK();
}

Status MemMetaImpl::Rename(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino,
                           std::string_view new_name, RenameFlag flags, RenameResult *result) {
  if (result) {
    *result = {};
  }
  if (new_name.size() > kMaxNameLength) {
    return Status::NameTooLong("target name exceeds maximum length");
  }

  // "." and ".." cannot be renamed
  if (old_name == "." || old_name == ".." || new_name == "." || new_name == "..") {
    return Status::Busy("cannot rename . or ..");
  }

  const SwordFsContext ctx = folly::fibers::local<SwordFsContext>();

  // The whole rename — validation, target removal, and the move itself —
  // runs as one transaction so concurrent observers can never see an
  // intermediate state (e.g. target unlinked but source not yet moved).
  // All structural bookkeeping (nlink, parent_ino, timestamps) lives in
  // the txn primitives; what remains here is policy.
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    // Both parents must exist and be directories.
    SwordFsInode old_parent;
    Status status = txn.LookupInode(old_parent_ino, &old_parent);
    if (!status.ok()) {
      return status;
    }
    if (!old_parent.IsDir()) {
      return Status::NotDirectory("old parent is not a directory");
    }
    SwordFsInode new_parent;
    status = txn.LookupInode(new_parent_ino, &new_parent);
    if (!status.ok()) {
      return status;
    }
    if (!new_parent.IsDir()) {
      return Status::NotDirectory("new parent is not a directory");
    }

    // Check write+execute permission on both parents
    if (!old_parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on old parent");
    }
    if (!new_parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on new parent");
    }

    SwordFsInode moved;
    status = txn.LookupEntry(old_parent_ino, old_name, &moved);
    if (!status.ok()) {
      return Status::NotFound("source entry not found");
    }

    // Sticky bit on the old parent: only root, the directory owner, or
    // the entry's owner can move entries out of it.
    if (!old_parent.CheckStickyDelete(ctx.uid, moved)) {
      return Status::Permission("sticky bit denied on source");
    }

    SwordFsInode existing;
    bool target_exists = txn.LookupEntry(new_parent_ino, new_name, &existing).ok();

    // Sticky bit on the new parent: overwriting or exchanging away an
    // existing entry removes it from that directory, so it requires the
    // same ownership as a delete.
    if (target_exists && !new_parent.CheckStickyDelete(ctx.uid, existing)) {
      return Status::Permission("sticky bit denied on target");
    }

    if (HasRenameFlag(flags, RenameFlag::kExchange)) {
      // RENAME_EXCHANGE requires the target to exist, and POSIX forbids
      // exchanging a directory with a non-directory.  Cycle prevention
      // is enforced by SwapEntries itself.
      if (!target_exists) {
        return Status::NotFound("target does not exist for RENAME_EXCHANGE");
      }
      if (existing.IsDir() != moved.IsDir()) {
        return Status::InvalidArgument("cannot exchange directory with non-directory");
      }
      return txn.SwapEntries(old_parent_ino, old_name, new_parent_ino, new_name);
    }

    // Cycle prevention, the self-rename no-op, victim type checks and
    // victim removal are all enforced by MoveEntry; kNoReplace simply
    // withholds overwrite permission.
    return txn.MoveEntry(old_parent_ino, old_name, new_parent_ino, new_name,
                         !HasRenameFlag(flags, RenameFlag::kNoReplace), result);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Rename: " << old_parent_ino << "/'" << old_name << "' -> " << new_parent_ino << "/'"
                      << new_name << "' failed: " << status.message();
    return status;
  }
  SWORDFS_LOG_DEBUG << "Rename: " << old_parent_ino << "/'" << old_name << "' -> " << new_parent_ino << "/'" << new_name
                    << "'";
  return Status::OK();
}

Status MemMetaImpl::SetAttr(InodeID ino, const struct stat *attr, SetAttrField fields, SwordFsInode *out) {
  SwordFsInode result;
  Status status = store_.Transact(
      [&](MemMetaTxn &txn) -> Status { return txn.SetAttr(ino, attr, fields, out ? &result : nullptr); });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "SetAttr: ino " << ino << " failed: " << status.message();
    return status;
  }
  if (out) {
    *out = result;
  }
  return Status::OK();
}

Status MemMetaImpl::Access(InodeID ino, int mask) {
  const SwordFsContext ctx = folly::fibers::local<SwordFsContext>();
  return store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode inode;
    Status status = txn.LookupInode(ino, &inode);
    if (!status.ok()) {
      return status;
    }

    if (!inode.CheckAccess(ctx.uid, ctx.gid, mask)) {
      return Status::Permission("access denied");
    }
    return Status::OK();
  });
}

Status MemMetaImpl::Open(InodeID ino) {
  const SwordFsContext ctx = folly::fibers::local<SwordFsContext>();
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode inode;
    Status status = txn.LookupInode(ino, &inode);
    if (!status.ok()) {
      return status;
    }

    // No `nlink == 0` check here. POSIX guarantees that an inode unlinked
    // while still open can be read/written through existing fds; the VFS
    // layer is also free to issue further Open() calls on the same inode
    // (re-open through /proc or other inode-by-number paths). Rejecting
    // `nlink == 0` here would break both cases. New opens by name go
    // through Lookup() at the VFS layer and never reach this code path
    // once the directory entry is gone.
    //
    // Only regular files can be opened (directories use OpenDir, symlinks
    // are resolved by the kernel).
    if (!inode.IsRegular()) {
      return Status::NotDirectory("not a regular file");
    }

    // Check read or write permission based on flags
    // (The kernel already passes filtered fi->flags to the FUSE daemon, so
    // O_RDONLY/O_WRONLY/O_RDWR are already set appropriately.)
    if (!inode.CheckAccess(ctx.uid, ctx.gid, R_OK)) {
      return Status::Permission("access denied");
    }

    // Update atime on the file.
    return txn.TouchInode(ino, SetAttrField::kAtime);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Open: ino " << ino << " failed: " << status.message();
  }
  return status;
}

Status MemMetaImpl::ReclaimInode(InodeID ino) {
  return store_.Transact([&](MemMetaTxn &txn) { return txn.ReclaimInode(ino); });
}

// ────────────────────────────────────────────────────────────────
// Directory operations
// ────────────────────────────────────────────────────────────────

Status MemMetaImpl::ReadDir(InodeID ino, std::vector<SwordFsEntry> *entries) {
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    // ListEntries itself validates that |ino| exists and is a directory
    // (NotFound / NotDirectory), and returns the full listing including
    // the synthetic "." and ".." entries.
    Status status = txn.ListEntries(ino, entries);
    if (!status.ok()) {
      return status;
    }
    // Reading directory contents updates atime on the directory.
    return txn.TouchInode(ino, SetAttrField::kAtime);
  });

  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "ReadDir: ino " << ino << " failed: " << status.message();
  }
  return status;
}

Status MemMetaImpl::MkDir(InodeID parent_ino, std::string_view name, mode_t mode, SwordFsInode *out) {
  if (name.size() > kMaxNameLength) {
    return Status::NameTooLong("directory name exceeds maximum length");
  }

  const SwordFsContext ctx = folly::fibers::local<SwordFsContext>();
  SwordFsInode child;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode parent;
    Status status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }

    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }

    mode_t dir_mode = (S_IFDIR | (mode & 0777));
    return txn.AddEntry(parent_ino, name, dir_mode, &child);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "MkDir: parent=" << parent_ino << " name='" << name << "' failed: " << status.message();
    return status;
  }

  SWORDFS_LOG_DEBUG << "MkDir: parent=" << parent_ino << " name='" << name << "' -> ino=" << child.ino;

  if (out) {
    *out = child;
  }
  return Status::OK();
}

Status MemMetaImpl::RmDir(InodeID parent_ino, std::string_view name) {
  // Cannot remove "." or ".."
  if (name == "." || name == "..") {
    return Status::InvalidArgument("cannot remove . or ..");
  }

  const SwordFsContext ctx = folly::fibers::local<SwordFsContext>();
  InodeID target_ino = 0;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode parent;
    Status status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }

    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }

    SwordFsInode target;
    status = txn.LookupEntry(parent_ino, name, &target);
    if (!status.ok()) {
      return status;
    }

    // Sticky bit on the parent directory: only root, the directory
    // owner, or the entry's owner can remove entries.
    if (!parent.CheckStickyDelete(ctx.uid, target)) {
      return Status::Permission("sticky bit denied");
    }

    if (!target.IsDir()) {
      return Status::NotDirectory("not a directory");
    }

    target_ino = target.ino;

    // Unlink detaches the entry, drops the parent's nlink (the ".." backlink),
    // and reclaims the now-empty directory inode.
    return txn.Unlink(parent_ino, name, nullptr);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "RmDir: parent=" << parent_ino << " name='" << name << "' failed: " << status.message();
    return status;
  }
  SWORDFS_LOG_DEBUG << "RmDir: parent=" << parent_ino << " name='" << name << "' ino=" << target_ino;
  return Status::OK();
}

Status MemMetaImpl::OpenDir(InodeID ino) {
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode dir;
    Status status = txn.LookupInode(ino, &dir);
    if (!status.ok()) {
      return status;
    }
    if (!dir.IsDir()) {
      return Status::NotDirectory("not a directory");
    }

    // Update atime on the directory.
    return txn.TouchInode(ino, SetAttrField::kAtime);
  });

  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "OpenDir: ino " << ino << " failed: " << status.message();
  }
  return status;
}

// ────────────────────────────────────────────────────────────────
// Link / symlink operations
// ────────────────────────────────────────────────────────────────

Status MemMetaImpl::Symlink(InodeID parent_ino, std::string_view name, const char *link, SwordFsInode *out) {
  if (name.size() > kMaxNameLength) {
    return Status::NameTooLong("symlink name exceeds maximum length");
  }

  const SwordFsContext ctx = folly::fibers::local<SwordFsContext>();
  SwordFsInode child;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode parent;
    Status status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }

    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }

    mode_t link_mode = (S_IFLNK | 0777);
    status = txn.AddEntry(parent_ino, name, link_mode, &child);
    if (!status.ok()) {
      return status;
    }

    status = txn.SetSymlinkTarget(child.ino, link);
    if (!status.ok()) {
      return status;
    }
    // Mirror the target/size into the local snapshot for the out params.
    child.symlink_target = link;
    child.attr.st_size = child.symlink_target.size();
    return Status::OK();
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Symlink: parent=" << parent_ino << " name='" << name << "' failed: " << status.message();
    return status;
  }

  if (out) {
    *out = child;
  }
  return Status::OK();
}

Status MemMetaImpl::Link(InodeID ino, InodeID newparent_ino, std::string_view newname, SwordFsInode *out) {
  if (newname.size() > kMaxNameLength) {
    return Status::NameTooLong("link name exceeds maximum length");
  }

  const SwordFsContext ctx = folly::fibers::local<SwordFsContext>();
  SwordFsInode inode;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    Status status = txn.LookupInode(ino, &inode);
    if (!status.ok()) {
      return status;
    }

    // Directories cannot be hard-linked (POSIX).
    if (inode.IsDir()) {
      return Status::NotPermitted("cannot hard-link directory");
    }

    SwordFsInode newparent;
    status = txn.LookupInode(newparent_ino, &newparent);
    if (!status.ok()) {
      return status;
    }
    if (!newparent.IsDir()) {
      return Status::NotDirectory("new parent is not a directory");
    }

    if (!newparent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on new parent");
    }

    return txn.LinkExistingEntry(newparent_ino, newname, ino, &inode);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Link: ino=" << ino << " parent=" << newparent_ino << " name='" << newname
                      << "' failed: " << status.message();
    return status;
  }
  if (out) {
    *out = inode;
  }
  return Status::OK();
}

Status MemMetaImpl::Readlink(InodeID ino, std::string *target) {
  SwordFsInode inode;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    Status status = txn.LookupInode(ino, &inode);
    if (!status.ok()) {
      return status;
    }

    if (!inode.IsSymlink()) {
      return Status::InvalidArgument("not a symbolic link");
    }

    *target = inode.symlink_target;
    return Status::OK();
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Readlink: ino " << ino << " failed: " << status.message();
  }
  return status;
}

// ────────────────────────────────────────────────────────────────
// Chunk metadata
// ────────────────────────────────────────────────────────────────

Status MemMetaImpl::AddChunk(InodeID ino, const ChunkMeta &cm) {
  return store_.Transact([&](MemMetaTxn &txn) { return txn.AddChunk(ino, cm); });
}

Status MemMetaImpl::FindChunk(InodeID ino, ChunkIndex idx, ChunkMeta *cm) {
  return store_.Transact([&](MemMetaTxn &txn) { return txn.FindChunk(ino, idx, cm); });
}

Status MemMetaImpl::ListChunks(InodeID ino, std::vector<ChunkMeta> *out) {
  return store_.Transact([&](MemMetaTxn &txn) { return txn.ListChunks(ino, out); });
}

Status MemMetaImpl::Truncate(InodeID ino, size_t size) {
  Status status = store_.Transact([&](MemMetaTxn &txn) { return txn.Truncate(ino, size); });
  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Truncate: ino " << ino << " to " << size << " failed: " << status.message();
  }
  return status;
}

// ────────────────────────────────────────────────────────────────
// Volume operations
// ────────────────────────────────────────────────────────────────

Status MemMetaImpl::StatFs(struct statvfs *stbuf) {
  std::memset(stbuf, 0, sizeof(*stbuf));

  Limits limits = GetLimits();
  stbuf->f_namemax = limits.max_name_length;
  stbuf->f_frsize = 4096;
  stbuf->f_bsize = 4096;
  // Report a large virtual capacity so df shows this mount.
  // 1 TiB = 1 * 1024 * 1024 * 1024 * 1024 / 4096 blocks
  stbuf->f_blocks = 268435456;  // ~1 TiB
  stbuf->f_bfree = 268435456;
  stbuf->f_bavail = 268435456;
  store_.Transact([&](MemMetaTxn &txn) { stbuf->f_files = txn.InodeCount(); });
  stbuf->f_ffree = limits.max_free_inodes;
  return Status::OK();
}

Limits MemMetaImpl::GetLimits() const {
  return Limits{kMaxNameLength, kMaxFreeInodes};
}

}  // namespace swordfs::metadata
