// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/mem/MemMetaImpl.hpp"

#include "metadata/Types.hpp"
#include "metadata/Utils.hpp"

#define FUSE_USE_VERSION 312
#include <dirent.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <fuse_lowlevel.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "utils/Logging.hpp"

namespace swordfs::metadata {

// ────────────────────────────────────────────────────────────────
// HandleTable
// ────────────────────────────────────────────────────────────────

uint64_t HandleTable::Alloc(InodeID ino) {
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t fh = next_fh_++;
  handles_[fh] = ino;
  return fh;
}

bool HandleTable::Release(uint64_t fh) {
  std::lock_guard<std::mutex> lock(mutex_);
  return handles_.erase(fh) > 0;
}

// ────────────────────────────────────────────────────────────────
// MemMetaImpl
// ────────────────────────────────────────────────────────────────

MemMetaImpl::MemMetaImpl() {
}

MemMetaImpl::~MemMetaImpl() {
}

void MemMetaImpl::KillSUID(struct stat* st) {
  if (st->st_mode & S_ISUID) st->st_mode &= ~S_ISUID;
  if (st->st_mode & S_ISGID) st->st_mode &= ~S_ISGID;
}

// ────────────────────────────────────────────────────────────────
// Public API
// ────────────────────────────────────────────────────────────────

Status MemMetaImpl::Lookup(InodeID parent_ino,
                           std::string_view name, InodeID* child_ino,
                           struct stat* attr) {
  SwordFsInode* inode = nullptr;
  Status status = store_.LookupEntry(parent_ino, name, &inode);
  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Lookup: parent=" << parent_ino << " name='" << name
                      << "' failed: " << status.message();
    return status;
  } else {
    SWORDFS_LOG_DEBUG << "Lookup: parent=" << parent_ino << " name='" << name
                      << "' -> ino=" << inode->ino;
  }

  // Increment lookup count so forget() can track when the kernel is done
  // referencing this inode.
  inode->nlookup++;

  if (child_ino) *child_ino = inode->ino;
  if (attr) *attr = inode->attr;

  return Status::OK();
}

Status MemMetaImpl::GetAttr(InodeID ino, struct stat* attr) {
  SwordFsInode* inode = nullptr;
  store_.LookupInode(ino, &inode);
  if (!inode) {
    SWORDFS_LOG_DEBUG << "GetAttr: ino " << ino << " not found";
    return Status::NotFound("inode not found");
  }
  *attr = inode->attr;
  return Status::OK();
}

Status MemMetaImpl::ReadDir(InodeID ino, std::vector<SwordFsEntry>* entries) {
  SwordFsInode* dir = nullptr;
  Status status = store_.LookupInode(ino, &dir);
  if (!status.ok()) {
    return status;
  } else if (!dir->IsDir()) {
    SWORDFS_LOG_ERROR << "ReadDir: ino " << ino << " is not a directory";
    return Status::NotDirectory("not a directory");
  }

  std::vector<std::pair<std::string, SwordFsInode*>> raw_entries;
  status = store_.ListEntries(ino, &raw_entries);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "ReadDir: ino " << ino << " dir map not found";
    return status;
  }

  for (const auto& [name, child_inode] : raw_entries) {
    entries->push_back(
        {name, ModeToDt(child_inode->attr.st_mode), child_inode->ino});
  }

  // Reading directory contents updates atime on the directory.
  dir->Touch(kAtime);
  return Status::OK();
}

Status MemMetaImpl::Create(InodeID parent_ino,
                           std::string_view name, mode_t mode,
                           InodeID* child_ino, struct stat* attr) {
  SwordFsInode* parent = nullptr;
  Status status = store_.LookupInode(parent_ino, &parent);
  if (!status.ok()) {
    return status;
  } else if (!parent->IsDir()) {
    SWORDFS_LOG_ERROR << "Create: parent " << parent_ino << " is not a directory";
    return Status::NotDirectory("parent is not a directory");
  }

  // Check permissions: need write+execute on the parent directory
  auto& ctx = folly::fibers::local<SwordFsContext>();
  if (!parent->CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
    return Status::Permission("access denied on parent");
  }

  mode_t file_mode = (S_IFREG | (mode & 0777));

  SwordFsInode* child = nullptr;
  status = store_.AddEntry(parent_ino, name, file_mode, 1, &child);
  if (!status.ok()) return status;

  // Parent directory mtime/ctime must be updated after a child is created.
  parent->Touch(kMtime | kCtime);

  if (child_ino) *child_ino = child->ino;
  if (attr) *attr = child->attr;

  SWORDFS_LOG_DEBUG << "Create: parent=" << parent_ino << " name='" << name
                    << "' -> ino=" << child->ino;
  return Status::OK();
}

Status MemMetaImpl::MkDir(InodeID parent_ino,
                          std::string_view name, mode_t mode,
                          InodeID* child_ino, struct stat* attr) {
  SwordFsInode* parent = nullptr;
  if (!store_.LookupInode(parent_ino, &parent).ok() || !parent ||
      !parent->IsDir()) {
    SWORDFS_LOG_ERROR << "MkDir: parent " << parent_ino << " is not a directory";
    return Status::NotDirectory("parent is not a directory");
  }

  auto& ctx = folly::fibers::local<SwordFsContext>();
  if (!parent->CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
    return Status::Permission("access denied on parent");
  }

  mode_t dir_mode = (S_IFDIR | (mode & 0777));

  SwordFsInode* child = nullptr;
  Status status = store_.AddEntry(parent_ino, name, dir_mode, 1, &child);
  if (!status.ok()) return status;

  // Increment parent nlink: the new subdirectory's ".." points back to the
  // parent, creating an additional hard link.
  if (parent) {
    parent->attr.st_nlink++;
  }

  parent->Touch(kMtime | kCtime);

  if (child_ino) *child_ino = child->ino;
  if (attr) *attr = child->attr;

  SWORDFS_LOG_DEBUG << "MkDir: parent=" << parent_ino << " name='" << name
                    << "' -> ino=" << child->ino;
  return Status::OK();
}

Status MemMetaImpl::Unlink(InodeID parent_ino,
                           std::string_view name) {
  SwordFsInode* parent = nullptr;
  if (!store_.LookupInode(parent_ino, &parent).ok() || !parent ||
      !parent->IsDir()) {
    SWORDFS_LOG_ERROR << "Unlink: parent " << parent_ino << " is not a directory";
    return Status::NotDirectory("parent is not a directory");
  }

  // Permission check on parent directory
  auto& ctx = folly::fibers::local<SwordFsContext>();
  if (!parent->CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
    return Status::Permission("access denied on parent");
  }

  // Sticky bit on directory: only the owner, directory owner, or root can
  // unlink entries.
  if (parent->attr.st_mode & S_ISVTX) {
    uid_t caller = folly::fibers::local<SwordFsContext>().uid;
    if (caller != 0 && caller != parent->attr.st_uid) {
      SwordFsInode* target = nullptr;
      if (store_.LookupEntry(parent_ino, name, &target).ok()) {
        if (target && caller != target->attr.st_uid) {
          return Status::Permission("sticky bit denied");
        }
      }
    }
  }

  std::string key(name);

  // Refuse to unlink "." or ".."
  if (key == "." || key == "..")
    return Status::InvalidArgument("cannot unlink . or ..");

  SwordFsInode* target = nullptr;
  Status status = store_.LookupEntry(parent_ino, name, &target);
  if (!status.ok()) return status;

  if (S_ISDIR(target->attr.st_mode)) {
    return Status::InvalidArgument("cannot unlink directory");
  }

  store_.RemoveEntry(parent_ino, name);

  if (parent) parent->Touch(kMtime | kCtime);

  SWORDFS_LOG_DEBUG << "Unlink: parent=" << parent_ino << " name='" << name
                    << "' ino=" << target->ino;
  return Status::OK();
}

Status MemMetaImpl::RmDir(InodeID parent_ino,
                          std::string_view name) {
  SwordFsInode* parent = nullptr;
  if (!store_.LookupInode(parent_ino, &parent).ok() || !parent ||
      !parent->IsDir()) {
    SWORDFS_LOG_ERROR << "RmDir: parent " << parent_ino << " is not a directory";
    return Status::NotDirectory("parent is not a directory");
  }

  auto& ctx = folly::fibers::local<SwordFsContext>();
  if (!parent->CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
    return Status::Permission("access denied on parent");
  }

  std::string key(name);

  // Cannot remove "." or ".."
  if (key == "." || key == "..")
    return Status::InvalidArgument("cannot remove . or ..");

  // Cannot remove the root directory by name
  if (parent_ino == FUSE_ROOT_ID && key == ".")
    return Status::Busy("root directory is busy");

  SwordFsInode* target = nullptr;
  Status status = store_.LookupEntry(parent_ino, name, &target);
  if (!status.ok()) return status;

  if (!S_ISDIR(target->attr.st_mode)) {
    return Status::NotDirectory("not a directory");
  }

  status = store_.RemoveEntry(parent_ino, name);
  if (!status.ok()) return status;

  // Decrement parent nlink: the removed subdirectory's ".." no longer points
  // back, so parent loses a hard link.
  if (parent) {
    parent->attr.st_nlink--;
  }

  parent->Touch(kMtime | kCtime);

  SWORDFS_LOG_DEBUG << "RmDir: parent=" << parent_ino << " name='" << name
                    << "' ino=" << target->ino;
  return Status::OK();
}

Status MemMetaImpl::Rename(InodeID old_parent_ino,
                           std::string_view old_name, InodeID new_parent_ino,
                           std::string_view new_name, unsigned int flags) {
  std::string old_key(old_name);
  std::string new_key(new_name);

  // "." and ".." cannot be renamed
  if (old_key == "." || old_key == ".." || new_key == "." ||
      new_key == "..") {
    return Status::Busy("cannot rename . or ..");
  }

  // Both parents must be directories
  SwordFsInode* op = nullptr;
  Status status = store_.LookupInode(old_parent_ino, &op);
  if (!status.ok() || !op || !op->IsDir()) {
    SWORDFS_LOG_ERROR << "Rename: old parent " << old_parent_ino
                      << " is not a directory";
    return Status::NotDirectory("old parent is not a directory");
  }
  SwordFsInode* np = nullptr;
  status = store_.LookupInode(new_parent_ino, &np);
  if (!status.ok() || !np || !np->IsDir()) {
    SWORDFS_LOG_ERROR << "Rename: new parent " << new_parent_ino
                      << " is not a directory";
    return Status::NotDirectory("new parent is not a directory");
  }

  // Check write+execute permission on both parents
  auto& ctx = folly::fibers::local<SwordFsContext>();
  if (op && !op->CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
    return Status::Permission("access denied on old parent");
  }
  if (np && !np->CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
    return Status::Permission("access denied on new parent");
  }

  SwordFsInode* moved = nullptr;
  status = store_.LookupEntry(old_parent_ino, old_name, &moved);
  if (!status.ok()) {
    return Status::NotFound("source entry not found");
  }

  InodeID ino = moved->ino;
  bool is_dir = S_ISDIR(moved->attr.st_mode);

  // Cannot move a directory into its own subtree
  if (is_dir && store_.IsDescendantOf(ino, new_parent_ino)) {
    return Status::InvalidArgument("cannot move directory into itself");
  }

  // ── RENAME flags handling ──────────────────────────────────────────

  SwordFsInode* existing = nullptr;
  bool target_exists = store_.LookupEntry(new_parent_ino, new_name, &existing).ok();

  // RENAME_NOREPLACE (1): fail if target already exists.
  if (flags & RENAME_NOREPLACE) {
    if (target_exists) {
      return Status::AlreadyExists("target exists and RENAME_NOREPLACE was set");
    }
  }

  // RENAME_EXCHANGE (2): atomically swap the two entries.
  if (flags & RENAME_EXCHANGE) {
    if (!target_exists) {
      return Status::NotFound("target does not exist for RENAME_EXCHANGE");
    }

    bool existing_is_dir = S_ISDIR(existing->attr.st_mode);
    if (existing_is_dir != is_dir) {
      return Status::InvalidArgument(
          "cannot exchange directory with non-directory");
    }

    Status status = store_.SwapEntries(old_parent_ino, old_name,
                                       new_parent_ino, new_name);
    if (!status.ok()) return status;

    // Adjust parent nlinks for cross-directory directory exchange.
    if (is_dir && old_parent_ino != new_parent_ino) {
      // Same net effect: src dir loses one parent, gains another;
      // dst dir vice versa.  Since both are directories moving between
      // different parents, both src and dst parent nlinks are unchanged
      // overall (each loses a subdirectory and gains one).
      // But each directory's own ".." entry must be updated:
      // The exchanged dirs now point to each other's parent.
      // In a pointer-based in-memory model this isn't tracked in nlink,
      // so no nlink adjustment is needed.
    }

    // Update timestamps.
    moved->Touch(kCtime);
    existing->Touch(kCtime);
    if (op) op->Touch(kMtime | kCtime);
    if (np) np->Touch(kMtime | kCtime);
    if (old_parent_ino != new_parent_ino && np != op) {
      // If new parent is different, and also different from old parent,
      // both parents' timestamps have already been touched above.
    }

    SWORDFS_LOG_DEBUG << "Rename EXCHANGE: " << old_parent_ino << "/'"
                      << old_key << "' <-> " << new_parent_ino << "/'"
                      << new_key << "'";
    return Status::OK();
  }

  // Handle overwrite of an existing target (normal rename)
  if (target_exists) {
    bool existing_is_dir = S_ISDIR(existing->attr.st_mode);

    // Cannot replace a directory with a file or vice versa
    if (existing_is_dir != is_dir) {
      return Status::InvalidArgument(
          "cannot replace directory with non-directory");
    }

    Status status = store_.RemoveEntry(new_parent_ino, new_name);
    if (!status.ok()) return status;

    if (existing_is_dir) {
      // Replacing an empty directory: decrement new_parent nlink
      if (np) np->attr.st_nlink--;
    }
  }

  status = store_.MoveEntry(old_parent_ino, old_name, new_parent_ino, new_name);
  if (!status.ok()) return status;

  // Cross-directory move of a directory: adjust parent nlinks
  if (is_dir && old_parent_ino != new_parent_ino) {
    if (op) op->attr.st_nlink--;
    if (np) np->attr.st_nlink++;
  }

  // ctime of the moved inode is updated on rename
  moved->Touch(kCtime);
  if (op) op->Touch(kMtime | kCtime);
  if (np) np->Touch(kMtime | kCtime);

  SWORDFS_LOG_DEBUG << "Rename: " << old_parent_ino << "/'" << old_name
                    << "' -> " << new_parent_ino << "/'" << new_name << "'";
  return Status::OK();
}

Status MemMetaImpl::SetAttr(InodeID ino,
                            const struct stat* attr, int to_set,
                            struct stat* out_attr) {
  SwordFsInode* inode = nullptr;
  store_.LookupInode(ino, &inode);
  if (!inode) {
    SWORDFS_LOG_ERROR << "SetAttr: ino " << ino << " not found";
    return Status::NotFound("inode not found");
  }

  auto& st = inode->attr;
  bool size_changed = false;
  bool owner_changed = false;

  if (to_set & FUSE_SET_ATTR_MODE) {
    st.st_mode = (st.st_mode & S_IFMT) | (attr->st_mode & 07777);
  }
  if (to_set & FUSE_SET_ATTR_UID) {
    if (st.st_uid != attr->st_uid) owner_changed = true;
    st.st_uid = attr->st_uid;
  }
  if (to_set & FUSE_SET_ATTR_GID) {
    if (st.st_gid != attr->st_gid) owner_changed = true;
    st.st_gid = attr->st_gid;
  }
  if (to_set & FUSE_SET_ATTR_SIZE) {
    if (st.st_size != attr->st_size) size_changed = true;
    st.st_size = attr->st_size;
  }
  if (to_set & FUSE_SET_ATTR_ATIME) {
    st.st_atime = attr->st_atime;
    st.st_atim.tv_nsec = attr->st_atim.tv_nsec;
  }
  if (to_set & FUSE_SET_ATTR_MTIME) {
    st.st_mtime = attr->st_mtime;
    st.st_mtim.tv_nsec = attr->st_mtim.tv_nsec;
  }
  if (to_set & FUSE_SET_ATTR_ATIME_NOW) {
    st.st_atime = ::time(nullptr);
    st.st_atim.tv_nsec = 0;
  }
  if (to_set & FUSE_SET_ATTR_MTIME_NOW) {
    st.st_mtime = ::time(nullptr);
    st.st_mtim.tv_nsec = 0;
  }
  if (to_set & FUSE_SET_ATTR_CTIME) {
    st.st_ctime = attr->st_ctime;
  }

  if (size_changed || owner_changed) {
    // Kill SUID/SGID if the owner or size changed (FUSE_CAP_HANDLE_KILLPRIV)
    KillSUID(&st);
  }

  // Update ctime unless it was explicitly set
  if (!(to_set & FUSE_SET_ATTR_CTIME)) {
    st.st_ctime = ::time(nullptr);
  }

  if (out_attr) *out_attr = st;
  return Status::OK();
}

Status MemMetaImpl::StatFs(struct statvfs* stbuf) {
  std::memset(stbuf, 0, sizeof(*stbuf));
  stbuf->f_namemax = 255;
  stbuf->f_frsize = 4096;
  stbuf->f_bsize = 4096;
  // Report a large virtual capacity so df shows this mount.
  // 1 TiB = 1 * 1024 * 1024 * 1024 * 1024 / 4096 blocks
  stbuf->f_blocks = 268435456;  // ~1 TiB
  stbuf->f_bfree = 268435456;
  stbuf->f_bavail = 268435456;
  stbuf->f_files = store_.InodeCount();
  stbuf->f_ffree = UINT64_MAX;  // unlimited for in-memory store
  return Status::OK();
}

Status MemMetaImpl::Access(InodeID ino,
                           int mask) {
  SwordFsInode* inode = nullptr;
  store_.LookupInode(ino, &inode);
  if (!inode) {
    return Status::NotFound("inode not found");
  }

  auto& ctx = folly::fibers::local<SwordFsContext>();
  if (!inode->CheckAccess(ctx.uid, ctx.gid, mask)) {
    return Status::Permission("access denied");
  }
  return Status::OK();
}

Status MemMetaImpl::Open(InodeID ino,
                         uint64_t* fh) {
  SwordFsInode* inode = nullptr;
  store_.LookupInode(ino, &inode);
  if (!inode) {
    SWORDFS_LOG_ERROR << "Open: ino " << ino << " not found";
    return Status::NotFound("inode not found");
  }

  // Only regular files can be opened (directories use OpenDir, symlinks are
  // resolved by the kernel).
  if (!S_ISREG(inode->attr.st_mode)) {
    SWORDFS_LOG_ERROR << "Open: ino " << ino << " is not a regular file";
    return Status::NotDirectory("not a regular file");
  }

  // Check read or write permission based on flags
  // (The kernel already passes filtered fi->flags to the FUSE daemon, so
  // O_RDONLY/O_WRONLY/O_RDWR are already set appropriately.)
  auto& ctx = folly::fibers::local<SwordFsContext>();
  if (!inode->CheckAccess(ctx.uid, ctx.gid, R_OK)) {
    return Status::Permission("access denied");
  }

  uint64_t handle = file_handles_.Alloc(ino);
  // Update atime on the file
  inode->attr.st_atime = ::time(nullptr);

  *fh = handle;
  return Status::OK();
}

Status MemMetaImpl::Release(uint64_t fh) {
  if (!file_handles_.Release(fh)) {
    SWORDFS_LOG_ERROR << "Release: unknown file handle " << fh;
    return Status::InvalidArgument("unknown file handle");
  }
  return Status::OK();
}

Status MemMetaImpl::OpenDir(InodeID ino,
                            uint64_t* fh) {
  SwordFsInode* dir = nullptr;
  if (!store_.LookupInode(ino, &dir).ok() || !dir ||
      !dir->IsDir()) {
    SWORDFS_LOG_ERROR << "OpenDir: ino " << ino << " is not a directory";
    return Status::NotDirectory("not a directory");
  }

  uint64_t handle = dir_handles_.Alloc(ino);
  // Update atime on the directory
  dir->Touch(kAtime);

  *fh = handle;
  return Status::OK();
}

Status MemMetaImpl::ReleaseDir(uint64_t fh) {
  if (!dir_handles_.Release(fh)) {
    SWORDFS_LOG_ERROR << "ReleaseDir: unknown dir handle " << fh;
    return Status::InvalidArgument("unknown dir handle");
  }
  return Status::OK();
}

Status MemMetaImpl::Forget(InodeID ino,
                           uint64_t nlookup) {
  SwordFsInode* inode = nullptr;
  store_.LookupInode(ino, &inode);
  if (!inode) return Status::OK();

  if (nlookup >= inode->nlookup) {
    inode->nlookup = 0;
  } else {
    inode->nlookup -= nlookup;
  }
  return Status::OK();
}

Status MemMetaImpl::AppendSlice(InodeID ino,
                                const storage::Slice& slice) {
  return store_.AppendSlice(ino, slice);
}

Status MemMetaImpl::GetSlices(InodeID ino,
                              storage::SliceList* out) {
  return store_.GetSlices(ino, out);
}

uint64_t MemMetaImpl::NextSliceID(InodeID ino) {
  return store_.NextSliceID(ino);
}

}  // namespace swordfs::metadata
