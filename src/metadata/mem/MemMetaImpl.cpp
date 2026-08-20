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

#include "metadata/Types.hpp"
#include "metadata/Utils.hpp"
#include "utils/Logging.hpp"

namespace swordfs::metadata {

namespace {

// MemMeta-specific filesystem limits.
constexpr size_t kMaxNameLength = 255;  // POSIX NAME_MAX
constexpr size_t kMaxFreeInodes = UINT64_MAX;

}  // namespace

// ────────────────────────────────────────────────────────────────
// MemMetaImpl
// ────────────────────────────────────────────────────────────────

MemMetaImpl::MemMetaImpl() {
}

MemMetaImpl::~MemMetaImpl() {
}

// Transaction model: every method below runs its mutation as a single
// store_.Transact() script (or delegates to a single store call, which
// is its own transaction), so each IMetaEngine operation is atomic.
//
// Discipline: a Transact() script contains ONLY MemMetaTxn primitives
// plus pure decisions on the value snapshots they return.  Logging,
// ambient state (folly fiber-local context), and anything else with
// side effects happens outside the transaction.  This keeps every
// script a pure read-snapshot -> compute -> write-back function — safe
// to re-execute, which is exactly what an optimistic-transaction
// (WATCH/MULTI/EXEC) KV backend does on conflict.

// ────────────────────────────────────────────────────────────────
// Public API
// ────────────────────────────────────────────────────────────────

Status MemMetaImpl::Lookup(InodeID parent_ino,
                           std::string_view name, InodeID *child_ino,
                           struct stat *attr) {
  SwordFsInode child;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    Status status = txn.LookupEntry(parent_ino, name, &child);
    if (!status.ok()) {
      return status;
    }
    // Increment lookup count so forget() can track when the kernel is
    // done referencing this inode.
    return txn.AddNlookup(child.ino, 1);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Lookup: parent=" << parent_ino << " name='" << name
                      << "' failed: " << status.message();
    return status;
  }
  SWORDFS_LOG_DEBUG << "Lookup: parent=" << parent_ino << " name='" << name
                    << "' -> ino=" << child.ino;

  if (child_ino) {
    *child_ino = child.ino;
  }
  if (attr) {
    *attr = child.attr;
  }
  return Status::OK();
}

Status MemMetaImpl::GetAttr(InodeID ino, struct stat *attr) {
  SwordFsInode inode;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    return txn.LookupInode(ino, &inode);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "GetAttr: ino " << ino
                      << " failed: " << status.message();
    return status;
  }
  *attr = inode.attr;
  return Status::OK();
}

Status MemMetaImpl::ReadDir(InodeID ino, std::vector<SwordFsEntry> *entries) {
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    Status status = txn.ListEntries(ino, entries);
    if (!status.ok()) {
      return status;
    }

    // Reading directory contents updates atime on the directory.
    return txn.TouchInode(ino, SetAttrField::kAtime);
  });

  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "ReadDir: ino " << ino
                      << " failed: " << status.message();
  }
  return status;
}

Status MemMetaImpl::Create(InodeID parent_ino,
                           std::string_view name, mode_t mode,
                           InodeID *child_ino, struct stat *attr) {
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

    status = txn.AddEntry(parent_ino, name, file_mode, 1, &child);
    if (!status.ok()) {
      return status;
    }

    // Parent directory mtime/ctime must be updated after a child is
    // created.
    return txn.TouchInode(parent_ino,
                          SetAttrField::kMtime | SetAttrField::kCtime);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Create: parent=" << parent_ino << " name='" << name
                      << "' failed: " << status.message();
    return status;
  }
  SWORDFS_LOG_DEBUG << "Create: parent=" << parent_ino << " name='" << name
                    << "' -> ino=" << child.ino;

  if (child_ino) {
    *child_ino = child.ino;
  }
  if (attr) {
    *attr = child.attr;
  }
  return Status::OK();
}

Status MemMetaImpl::MkDir(InodeID parent_ino,
                          std::string_view name, mode_t mode,
                          InodeID *child_ino, struct stat *attr) {
  if (name.size() > kMaxNameLength) {
    return Status::NameTooLong("directory name exceeds maximum length");
  }

  const SwordFsContext ctx = folly::fibers::local<SwordFsContext>();
  SwordFsInode child;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode parent;
    if (!txn.LookupInode(parent_ino, &parent).ok() || !parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }

    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }

    mode_t dir_mode = (S_IFDIR | (mode & 0777));

    Status status = txn.AddEntry(parent_ino, name, dir_mode, 1, &child);
    if (!status.ok()) {
      return status;
    }

    // Increment parent nlink: the new subdirectory's ".." points back to
    // the parent, creating an additional hard link.
    status = txn.AdjustNlink(parent_ino, 1);
    if (!status.ok()) {
      return status;
    }

    return txn.TouchInode(parent_ino,
                          SetAttrField::kMtime | SetAttrField::kCtime);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "MkDir: parent=" << parent_ino << " name='" << name
                      << "' failed: " << status.message();
    return status;
  }
  SWORDFS_LOG_DEBUG << "MkDir: parent=" << parent_ino << " name='" << name
                    << "' -> ino=" << child.ino;

  if (child_ino) {
    *child_ino = child.ino;
  }
  if (attr) {
    *attr = child.attr;
  }
  return Status::OK();
}

Status MemMetaImpl::Unlink(InodeID parent_ino,
                           std::string_view name,
                           nlink_t *post_nlink) {
  std::string key(name);

  // Refuse to unlink "." or ".."
  if (key == "." || key == "..") {
    return Status::InvalidArgument("cannot unlink . or ..");
  }

  const SwordFsContext ctx = folly::fibers::local<SwordFsContext>();
  InodeID target_ino = 0;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode parent;
    if (!txn.LookupInode(parent_ino, &parent).ok() || !parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }

    // Permission check on parent directory
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }

    SwordFsInode target;
    Status status = txn.LookupEntry(parent_ino, name, &target);
    if (!status.ok()) {
      return status;
    }

    // Sticky bit on directory: only the owner, directory owner, or root
    // can unlink entries.
    if ((parent.attr.st_mode & S_ISVTX) && ctx.uid != 0 &&
        ctx.uid != parent.attr.st_uid && ctx.uid != target.attr.st_uid) {
      return Status::Permission("sticky bit denied");
    }

    if (S_ISDIR(target.attr.st_mode)) {
      return Status::InvalidArgument("cannot unlink directory");
    }

    target_ino = target.ino;

    // Unlink only detaches the directory entry and decrements nlink; the
    // transaction hands back the authoritative post-decrement nlink in
    // *post_nlink so the caller doesn't have to re-read it (avoiding the
    // TOCTOU race that an unlink-before-read decision would have).
    status = txn.Unlink(parent_ino, name, post_nlink);
    if (!status.ok()) {
      return status;
    }

    return txn.TouchInode(parent_ino,
                          SetAttrField::kMtime | SetAttrField::kCtime);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Unlink: parent=" << parent_ino << " name='" << name
                      << "' failed: " << status.message();
    return status;
  }
  SWORDFS_LOG_DEBUG << "Unlink: parent=" << parent_ino << " name='" << name
                    << "' ino=" << target_ino;
  return Status::OK();
}

Status MemMetaImpl::RmDir(InodeID parent_ino,
                          std::string_view name) {
  std::string key(name);

  // Cannot remove "." or ".."
  if (key == "." || key == "..") {
    return Status::InvalidArgument("cannot remove . or ..");
  }

  // Cannot remove the root directory by name
  if (parent_ino == kRootInodeId && key == ".") {
    return Status::Busy("root directory is busy");
  }

  const SwordFsContext ctx = folly::fibers::local<SwordFsContext>();
  InodeID target_ino = 0;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode parent;
    if (!txn.LookupInode(parent_ino, &parent).ok() || !parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }

    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }

    SwordFsInode target;
    Status status = txn.LookupEntry(parent_ino, name, &target);
    if (!status.ok()) {
      return status;
    }

    if (!S_ISDIR(target.attr.st_mode)) {
      return Status::NotDirectory("not a directory");
    }

    target_ino = target.ino;

    status = txn.Unlink(parent_ino, name, nullptr);
    if (!status.ok()) {
      return status;
    }

    // Decrement parent nlink: the removed subdirectory's ".." no longer
    // points back, so parent loses a hard link.
    status = txn.AdjustNlink(parent_ino, -1);
    if (!status.ok()) {
      return status;
    }

    return txn.TouchInode(parent_ino,
                          SetAttrField::kMtime | SetAttrField::kCtime);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "RmDir: parent=" << parent_ino << " name='" << name
                      << "' failed: " << status.message();
    return status;
  }
  SWORDFS_LOG_DEBUG << "RmDir: parent=" << parent_ino << " name='" << name
                    << "' ino=" << target_ino;
  return Status::OK();
}

Status MemMetaImpl::Rename(InodeID old_parent_ino,
                           std::string_view old_name, InodeID new_parent_ino,
                           std::string_view new_name, RenameFlag flags) {
  std::string old_key(old_name);
  std::string new_key(new_name);

  if (new_name.size() > kMaxNameLength) {
    return Status::NameTooLong("target name exceeds maximum length");
  }

  // "." and ".." cannot be renamed
  if (old_key == "." || old_key == ".." || new_key == "." ||
      new_key == "..") {
    return Status::Busy("cannot rename . or ..");
  }

  const SwordFsContext ctx = folly::fibers::local<SwordFsContext>();

  // The whole rename — validation, target removal, and the move itself —
  // runs as one transaction so concurrent observers can never see an
  // intermediate state (e.g. target unlinked but source not yet moved).
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    // Both parents must be directories
    SwordFsInode op;
    Status status = txn.LookupInode(old_parent_ino, &op);
    if (!status.ok() || !op.IsDir()) {
      return Status::NotDirectory("old parent is not a directory");
    }
    SwordFsInode np;
    status = txn.LookupInode(new_parent_ino, &np);
    if (!status.ok() || !np.IsDir()) {
      return Status::NotDirectory("new parent is not a directory");
    }

    // Check write+execute permission on both parents
    if (!op.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on old parent");
    }
    if (!np.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on new parent");
    }

    SwordFsInode moved;
    status = txn.LookupEntry(old_parent_ino, old_name, &moved);
    if (!status.ok()) {
      return Status::NotFound("source entry not found");
    }

    InodeID ino = moved.ino;
    bool is_dir = S_ISDIR(moved.attr.st_mode);

    // Cannot move a directory into itself or its own subtree.  The
    // descendant check alone misses the direct self-move
    // (new_parent_ino == ino), which would create a directory cycle.
    if (is_dir &&
        (new_parent_ino == ino || txn.IsDescendantOf(ino, new_parent_ino))) {
      return Status::InvalidArgument("cannot move directory into itself");
    }

    // ── RENAME flags handling ────────────────────────────────────────

    SwordFsInode existing;
    bool target_exists =
        txn.LookupEntry(new_parent_ino, new_name, &existing).ok();

    if (HasRenameFlag(flags, RenameFlag::kNoReplace)) {
      if (target_exists) {
        return Status::AlreadyExists(
            "target exists and RenameFlag::kNoReplace was set");
      }
    }

    if (HasRenameFlag(flags, RenameFlag::kExchange)) {
      if (!target_exists) {
        return Status::NotFound("target does not exist for RENAME_EXCHANGE");
      }

      bool existing_is_dir = S_ISDIR(existing.attr.st_mode);
      if (existing_is_dir != is_dir) {
        return Status::InvalidArgument(
            "cannot exchange directory with non-directory");
      }

      status = txn.SwapEntries(old_parent_ino, old_name, new_parent_ino,
                               new_name);
      if (!status.ok()) {
        return status;
      }

      // Cross-directory directory exchange needs no nlink adjustment:
      // both parents lose a subdirectory and gain one.  parent_ino of
      // the exchanged inodes is updated by SwapEntries.

      // Update timestamps.
      txn.TouchInode(ino, SetAttrField::kCtime);
      txn.TouchInode(existing.ino, SetAttrField::kCtime);
      txn.TouchInode(old_parent_ino,
                     SetAttrField::kMtime | SetAttrField::kCtime);
      txn.TouchInode(new_parent_ino,
                     SetAttrField::kMtime | SetAttrField::kCtime);
      return Status::OK();
    }

    // Handle overwrite of an existing target (normal rename)
    if (target_exists) {
      // Rename to self is a no-op.
      if (existing.ino == ino) {
        return Status::OK();
      }

      bool existing_is_dir = S_ISDIR(existing.attr.st_mode);

      // Cannot replace a directory with a file or vice versa
      if (existing_is_dir != is_dir) {
        if (existing_is_dir) {
          return Status::IsDirectory("target is a directory");
        }
        return Status::NotDirectory("target is not a directory");
      }

      status = txn.Unlink(new_parent_ino, new_name, nullptr);
      if (!status.ok()) {
        return status;
      }

      if (existing_is_dir) {
        // Replacing an empty directory: decrement new_parent nlink
        status = txn.AdjustNlink(new_parent_ino, -1);
        if (!status.ok()) {
          return status;
        }
      } else {
        // Overwriting a file: `Unlink` only detaches the directory entry;
        // the inode (and its chunks) are still alive. Free the inode here.
        // The VFS layer (above) is responsible for issuing data-engine
        // deletes before this returns, since by definition we are inside
        // a rename and there cannot be any open fd on the overwritten
        // path component (the original caller already removed the only
        // link).
        txn.ReclaimInode(existing.ino);
      }
    }

    status = txn.MoveEntry(old_parent_ino, old_name, new_parent_ino,
                           new_name);
    if (!status.ok()) {
      return status;
    }

    // Cross-directory move of a directory: adjust parent nlinks
    if (is_dir && old_parent_ino != new_parent_ino) {
      status = txn.AdjustNlink(old_parent_ino, -1);
      if (!status.ok()) {
        return status;
      }
      status = txn.AdjustNlink(new_parent_ino, 1);
      if (!status.ok()) {
        return status;
      }
    }

    // ctime of the moved inode is updated on rename
    txn.TouchInode(ino, SetAttrField::kCtime);
    txn.TouchInode(old_parent_ino, SetAttrField::kMtime | SetAttrField::kCtime);
    txn.TouchInode(new_parent_ino, SetAttrField::kMtime | SetAttrField::kCtime);
    return Status::OK();
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Rename: " << old_parent_ino << "/'" << old_name
                      << "' -> " << new_parent_ino << "/'" << new_name
                      << "' failed: " << status.message();
    return status;
  }
  SWORDFS_LOG_DEBUG << "Rename: " << old_parent_ino << "/'" << old_name
                    << "' -> " << new_parent_ino << "/'" << new_name << "'";
  return Status::OK();
}

Status MemMetaImpl::SetAttr(InodeID ino,
                            const struct stat *attr, SetAttrField fields,
                            struct stat *out_attr) {
  struct stat result {};
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode inode;
    Status status = txn.LookupInode(ino, &inode);
    if (!status.ok()) {
      return status;
    }

    // Read-modify-write on a local snapshot; written back once below.
    struct stat st = inode.attr;
    bool owner_changed = false;

    if (HasSetAttrField(fields, SetAttrField::kMode)) {
      st.st_mode = (st.st_mode & S_IFMT) | (attr->st_mode & 07777);
    }
    if (HasSetAttrField(fields, SetAttrField::kUid)) {
      if (st.st_uid != attr->st_uid) {
        owner_changed = true;
      }
      st.st_uid = attr->st_uid;
    }
    if (HasSetAttrField(fields, SetAttrField::kGid)) {
      if (st.st_gid != attr->st_gid) {
        owner_changed = true;
      }
      st.st_gid = attr->st_gid;
    }
    if (HasSetAttrField(fields, SetAttrField::kSize)) {
      if (st.st_size != attr->st_size) {
        st.st_size = attr->st_size;
        // Size changes clear SUID/SGID (FUSE_CAP_HANDLE_KILLPRIV).
        KillSUID(&st);
      }
      // Drop out-of-range chunk metadata.
      status = txn.TruncateChunks(ino, static_cast<size_t>(attr->st_size));
      if (!status.ok()) {
        return status;
      }
    }
    if (HasSetAttrField(fields, SetAttrField::kAtime)) {
      st.st_atime = attr->st_atime;
      st.st_atim.tv_nsec = attr->st_atim.tv_nsec;
    }
    if (HasSetAttrField(fields, SetAttrField::kMtime)) {
      st.st_mtime = attr->st_mtime;
      st.st_mtim.tv_nsec = attr->st_mtim.tv_nsec;
    }
    if (HasSetAttrField(fields, SetAttrField::kAtimeNow)) {
      st.st_atime = ::time(nullptr);
      st.st_atim.tv_nsec = 0;
    }
    if (HasSetAttrField(fields, SetAttrField::kMtimeNow)) {
      st.st_mtime = ::time(nullptr);
      st.st_mtim.tv_nsec = 0;
    }
    if (HasSetAttrField(fields, SetAttrField::kCtime)) {
      st.st_ctime = attr->st_ctime;
    }

    if (owner_changed) {
      // Kill SUID/SGID if the owner changed (FUSE_CAP_HANDLE_KILLPRIV).
      KillSUID(&st);
    }

    // Update ctime unless it was explicitly set
    if (!HasSetAttrField(fields, SetAttrField::kCtime)) {
      st.st_ctime = ::time(nullptr);
    }

    // Single write-back makes the whole SetAttr atomic.
    status = txn.WriteAttr(ino, &st);
    if (!status.ok()) {
      return status;
    }

    result = st;
    return Status::OK();
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "SetAttr: ino " << ino
                      << " failed: " << status.message();
    return status;
  }
  if (out_attr) {
    *out_attr = result;
  }
  return Status::OK();
}

Status MemMetaImpl::Symlink(InodeID parent_ino,
                            std::string_view name, const char *link,
                            InodeID *child_ino, struct stat *attr) {
  if (name.size() > kMaxNameLength) {
    return Status::NameTooLong("symlink name exceeds maximum length");
  }

  const SwordFsContext ctx = folly::fibers::local<SwordFsContext>();
  SwordFsInode child;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode parent;
    Status status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok() || !parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }

    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }

    mode_t link_mode = (S_IFLNK | 0777);
    status = txn.AddEntry(parent_ino, name, link_mode, 1, &child);
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

    return txn.TouchInode(parent_ino,
                          SetAttrField::kMtime | SetAttrField::kCtime);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Symlink: parent=" << parent_ino << " name='" << name
                      << "' failed: " << status.message();
    return status;
  }

  if (child_ino) {
    *child_ino = child.ino;
  }
  if (attr) {
    *attr = child.attr;
  }
  return Status::OK();
}

Status MemMetaImpl::Link(InodeID ino, InodeID newparent_ino,
                         std::string_view newname, struct stat *attr) {
  if (newname.size() > kMaxNameLength) {
    return Status::NameTooLong("link name exceeds maximum length");
  }

  const SwordFsContext ctx = folly::fibers::local<SwordFsContext>();
  SwordFsInode inode;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    Status status = txn.LookupInode(ino, &inode);
    if (!status.ok()) {
      return Status::NotFound("source inode not found");
    }

    // Directories cannot be hard-linked (POSIX).
    if (inode.IsDir()) {
      return Status::NotPermitted("cannot hard-link directory");
    }

    SwordFsInode newparent;
    status = txn.LookupInode(newparent_ino, &newparent);
    if (!status.ok() || !newparent.IsDir()) {
      return Status::NotDirectory("new parent is not a directory");
    }

    if (!newparent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on new parent");
    }

    status = txn.LinkExistingEntry(newparent_ino, newname, ino);
    if (!status.ok()) {
      return status;
    }

    txn.TouchInode(newparent_ino, SetAttrField::kMtime | SetAttrField::kCtime);
    txn.TouchInode(ino, SetAttrField::kCtime);

    // Mirror the link side effects into the local snapshot for the out
    // param (same pattern as Symlink) instead of re-reading the inode.
    inode.attr.st_nlink++;
    inode.attr.st_ctime = ::time(nullptr);
    return Status::OK();
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Link: ino=" << ino << " parent=" << newparent_ino
                      << " name='" << newname
                      << "' failed: " << status.message();
    return status;
  }
  if (attr) {
    *attr = inode.attr;
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

    if (!S_ISLNK(inode.attr.st_mode)) {
      return Status::InvalidArgument("not a symbolic link");
    }

    *target = inode.symlink_target;
    return Status::OK();
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Readlink: ino " << ino
                      << " failed: " << status.message();
  }
  return status;
}

Status MemMetaImpl::StatFs(struct statvfs *stbuf) {
  std::memset(stbuf, 0, sizeof(*stbuf));

  struct Limits limits = GetLimits();
  stbuf->f_namemax = limits.max_name_length;
  stbuf->f_frsize = 4096;
  stbuf->f_bsize = 4096;
  // Report a large virtual capacity so df shows this mount.
  // 1 TiB = 1 * 1024 * 1024 * 1024 * 1024 / 4096 blocks
  stbuf->f_blocks = 268435456;  // ~1 TiB
  stbuf->f_bfree = 268435456;
  stbuf->f_bavail = 268435456;
  store_.Transact([&](MemMetaTxn &txn) {
    stbuf->f_files = txn.InodeCount();
  });
  stbuf->f_ffree = limits.max_free_inodes;
  return Status::OK();
}

Limits MemMetaImpl::GetLimits() {
  return Limits{kMaxNameLength, kMaxFreeInodes};
}

Status MemMetaImpl::Access(InodeID ino,
                           int mask) {
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
    if (!S_ISREG(inode.attr.st_mode)) {
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
    SWORDFS_LOG_DEBUG << "Open: ino " << ino
                      << " failed: " << status.message();
  }
  return status;
}

Status MemMetaImpl::ReclaimInode(InodeID ino) {
  return store_.Transact([&](MemMetaTxn &txn) { return txn.ReclaimInode(ino); });
}

Status MemMetaImpl::ListChunks(InodeID ino, std::vector<ChunkMeta> *out) {
  return store_.Transact([&](MemMetaTxn &txn) { return txn.ListChunks(ino, out); });
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
    SWORDFS_LOG_ERROR << "OpenDir: ino " << ino
                      << " failed: " << status.message();
  }
  return status;
}

Status MemMetaImpl::Forget(InodeID ino,
                           uint64_t nlookup) {
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    // Subtract with saturation at zero.
    return txn.AddNlookup(ino, -static_cast<int64_t>(nlookup));
  });
  // A missing inode is fine (the kernel may forget an inode we already
  // reclaimed).
  if (status.IsNotFound()) {
    return Status::OK();
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
  return store_.Transact(
      [&](MemMetaTxn &txn) { return txn.FindChunk(ino, idx, cm); });
}

// TruncateInTxn runs inside an existing transaction (the caller's
// Transact() scope, e.g. from the public Truncate below).
Status MemMetaImpl::TruncateInTxn(MemMetaTxn &txn, InodeID ino,
                                  size_t size) {
  SwordFsInode inode;
  Status status = txn.LookupInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }

  struct stat st = inode.attr;
  if (st.st_size != static_cast<off_t>(size)) {
    st.st_size = static_cast<off_t>(size);
    // Size changes clear SUID/SGID (FUSE_CAP_HANDLE_KILLPRIV).
    KillSUID(&st);
    st.st_ctime = ::time(nullptr);
    status = txn.WriteAttr(ino, &st);
    if (!status.ok()) {
      return status;
    }
  }

  return txn.TruncateChunks(ino, size);
}

Status MemMetaImpl::Truncate(InodeID ino, size_t size) {
  Status status = store_.Transact(
      [&](MemMetaTxn &txn) { return TruncateInTxn(txn, ino, size); });
  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Truncate: ino " << ino << " to " << size
                      << " failed: " << status.message();
  }
  return status;
}

}  // namespace swordfs::metadata
