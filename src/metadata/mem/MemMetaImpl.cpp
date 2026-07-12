// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/mem/MemMetaImpl.hpp"

#include "metadata/Types.hpp"
#include "metadata/Utils.hpp"

#define FUSE_USE_VERSION 312
#include <dirent.h>
#include <fuse_lowlevel.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include <folly/fibers/FiberManagerInternal.h>

#include "utils/Logging.hpp"

namespace swordfs::metadata {

MemMetaImpl::MemMetaImpl() {
}

MemMetaImpl::~MemMetaImpl() {
}

uint64_t MemMetaImpl::AllocFh() { return next_fh_++; }

void MemMetaImpl::KillSUID(struct stat* st) {
  if (st->st_mode & S_ISUID) st->st_mode &= ~S_ISUID;
  if (st->st_mode & S_ISGID) st->st_mode &= ~S_ISGID;
}

int MemMetaImpl::Access(const struct stat* st,
                          int mask) const {
  // Root always has full access
  auto& ctx = folly::fibers::local<SwordFsContext>();
  if (ctx.uid == 0) return 0;

  uid_t uid = ctx.uid;
  gid_t gid = ctx.gid;

  unsigned int access_bits = 0;
  if (uid == st->st_uid) {
    // Owner
    access_bits = (st->st_mode & S_IRWXU) >> 6;
  } else if (gid == st->st_gid) {
    // Group
    access_bits = (st->st_mode & S_IRWXG) >> 3;
  } else {
    // Other
    access_bits = st->st_mode & S_IRWXO;
  }

  if ((mask & R_OK) && !(access_bits & R_OK)) return -EACCES;
  if ((mask & W_OK) && !(access_bits & W_OK)) return -EACCES;
  if ((mask & X_OK) && !(access_bits & X_OK)) return -EACCES;
  return 0;
}

Status MemMetaImpl::Lookup(InodeID parent,
                            std::string_view name, InodeID* child,
                            struct stat* attr) {
  std::lock_guard<std::mutex> lock(mutex_);

  SwordFsInode* parent_inode = nullptr;
  Status status = store_.LookupInode(parent, &parent_inode);
  if (!status.ok() || !parent_inode || !parent_inode->IsDir()) {
    SWORDFS_LOG_ERROR << "Lookup: parent " << parent << " is not a directory";
    return Status::NotDirectory("parent is not a directory");
  }

  SwordFsInode* inode = nullptr;
  status = store_.LookupEntry(parent, name, &inode);
  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Lookup: parent=" << parent << " name='" << name
                      << "' failed: " << status.message();
    return status;
  }

  // Increment lookup count so forget() can track when the kernel is done
  // referencing this inode.
  inode->nlookup++;

  if (child) *child = inode->ino;
  if (attr) *attr = inode->attr;

  SWORDFS_LOG_DEBUG << "Lookup: parent=" << parent << " name='" << name
                    << "' -> ino=" << inode->ino;
  return Status::OK();
}

Status MemMetaImpl::GetAttr(InodeID ino,
                             struct stat* attr) {
  std::lock_guard<std::mutex> lock(mutex_);

  SwordFsInode* inode = nullptr;
  store_.LookupInode(ino, &inode);
  if (!inode) {
    SWORDFS_LOG_DEBUG << "GetAttr: ino " << ino << " not found";
    return Status::NotFound("inode not found");
  }
  *attr = inode->attr;
  return Status::OK();
}

Status MemMetaImpl::ReadDir(InodeID ino,
                             std::vector<SwordFsEntry>* entries) {
  std::lock_guard<std::mutex> lock(mutex_);

  SwordFsInode* dir_inode = nullptr;
  if (!store_.LookupInode(ino, &dir_inode).ok() || !dir_inode ||
      !dir_inode->IsDir()) {
    SWORDFS_LOG_ERROR << "ReadDir: ino " << ino << " is not a directory";
    return Status::NotDirectory("not a directory");
  }

  std::vector<std::pair<std::string, SwordFsInode*>> raw_entries;
  Status status = store_.ListEntries(ino, &raw_entries);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "ReadDir: ino " << ino << " dir map not found";
    return status;
  }

  for (const auto& [name, child_inode] : raw_entries) {
    entries->push_back(
        {name, ModeToDt(child_inode->attr.st_mode), child_inode->ino});
  }

  // Reading directory contents updates atime on the directory.
  dir_inode->Touch(kAtime);
  return Status::OK();
}

Status MemMetaImpl::Create(InodeID parent,
                            std::string_view name, mode_t mode,
                            InodeID* child_ino, struct stat* attr) {
  std::lock_guard<std::mutex> lock(mutex_);

  SwordFsInode* parent_inode = nullptr;
  store_.LookupInode(parent, &parent_inode);
  if (!parent_inode) {
    return Status::NotFound("parent inode not found");
  }

  if (!S_ISDIR(parent_inode->attr.st_mode)) {
    SWORDFS_LOG_ERROR << "Create: parent " << parent << " is not a directory";
    return Status::NotDirectory("parent is not a directory");
  }

  // Check permissions: need write+execute on the parent directory
  int r = Access(&parent_inode->attr, W_OK | X_OK);
  if (r != 0) return Status::Permission("access denied on parent");

  mode_t file_mode = (S_IFREG | (mode & 0777));

  SwordFsInode* child_inode = nullptr;
  Status status = store_.AddEntry(parent, name, file_mode, 1, &child_inode);
  if (!status.ok()) return status;

  // Parent directory mtime/ctime must be updated after a child is created.
  parent_inode->Touch(kMtime | kCtime);

  if (child_ino) *child_ino = child_inode->ino;
  if (attr) *attr = child_inode->attr;

  SWORDFS_LOG_DEBUG << "Create: parent=" << parent << " name='" << name
                    << "' -> ino=" << child_inode->ino;
  return Status::OK();
}

Status MemMetaImpl::MkDir(InodeID parent,
                           std::string_view name, mode_t mode,
                           InodeID* child_ino, struct stat* attr) {
  std::lock_guard<std::mutex> lock(mutex_);

  SwordFsInode* parent_in = nullptr;
  if (!store_.LookupInode(parent, &parent_in).ok() || !parent_in ||
      !parent_in->IsDir()) {
    SWORDFS_LOG_ERROR << "MkDir: parent " << parent << " is not a directory";
    return Status::NotDirectory("parent is not a directory");
  }

  {
    int r = Access(&parent_in->attr, W_OK | X_OK);
    if (r != 0) return Status::Permission("access denied on parent");
  }

  mode_t dir_mode = (S_IFDIR | (mode & 0777));

  SwordFsInode* child_inode = nullptr;
  Status status = store_.AddEntry(parent, name, dir_mode, 1, &child_inode);
  if (!status.ok()) return status;

  // Increment parent nlink: the new subdirectory's ".." points back to the
  // parent, creating an additional hard link.
  if (parent_in) {
    parent_in->attr.st_nlink++;
  }

  parent_in->Touch(kMtime | kCtime);

  if (child_ino) *child_ino = child_inode->ino;
  if (attr) *attr = child_inode->attr;

  SWORDFS_LOG_DEBUG << "MkDir: parent=" << parent << " name='" << name
                    << "' -> ino=" << child_inode->ino;
  return Status::OK();
}

Status MemMetaImpl::Unlink(InodeID parent,
                            std::string_view name) {
  std::lock_guard<std::mutex> lock(mutex_);

  SwordFsInode* parent_in = nullptr;
  if (!store_.LookupInode(parent, &parent_in).ok() || !parent_in ||
      !parent_in->IsDir()) {
    SWORDFS_LOG_ERROR << "Unlink: parent " << parent << " is not a directory";
    return Status::NotDirectory("parent is not a directory");
  }

  // Permission check on parent directory
  {
    int r = Access(&parent_in->attr, W_OK | X_OK);
    if (r != 0) return Status::Permission("access denied on parent");

    // Sticky bit on directory: only the owner, directory owner, or root can
    // unlink entries.
    if (parent_in->attr.st_mode & S_ISVTX) {
      uid_t caller = folly::fibers::local<SwordFsContext>().uid;
      if (caller != 0 && caller != parent_in->attr.st_uid) {
        SwordFsInode* target = nullptr;
        if (store_.LookupEntry(parent, name, &target).ok()) {
          if (target && caller != target->attr.st_uid) {
            return Status::Permission("sticky bit denied");
          }
        }
      }
    }
  }

  std::string key(name);

  // Refuse to unlink "." or ".."
  if (key == "." || key == "..")
    return Status::InvalidArgument("cannot unlink . or ..");

  SwordFsInode* target_inode = nullptr;
  Status status = store_.LookupEntry(parent, name, &target_inode);
  if (!status.ok()) return status;

  if (S_ISDIR(target_inode->attr.st_mode)) {
    return Status::InvalidArgument("cannot unlink directory");
  }

  store_.RemoveEntry(parent, name, true);

  if (parent_in) parent_in->Touch(kMtime | kCtime);

  SWORDFS_LOG_DEBUG << "Unlink: parent=" << parent << " name='" << name
                    << "' ino=" << target_inode->ino;
  return Status::OK();
}

Status MemMetaImpl::RmDir(InodeID parent,
                           std::string_view name) {
  std::lock_guard<std::mutex> lock(mutex_);

  SwordFsInode* parent_in = nullptr;
  if (!store_.LookupInode(parent, &parent_in).ok() || !parent_in ||
      !parent_in->IsDir()) {
    SWORDFS_LOG_ERROR << "RmDir: parent " << parent << " is not a directory";
    return Status::NotDirectory("parent is not a directory");
  }

  {
    int r = Access(&parent_in->attr, W_OK | X_OK);
    if (r != 0) return Status::Permission("access denied on parent");
  }

  std::string key(name);

  // Cannot remove "." or ".."
  if (key == "." || key == "..")
    return Status::InvalidArgument("cannot remove . or ..");

  // Cannot remove the root directory by name
  if (parent == FUSE_ROOT_ID && key == ".")
    return Status::Busy("root directory is busy");

  SwordFsInode* target_inode = nullptr;
  Status status = store_.LookupEntry(parent, name, &target_inode);
  if (!status.ok()) return status;

  if (!S_ISDIR(target_inode->attr.st_mode)) {
    return Status::NotDirectory("not a directory");
  }

  status = store_.RemoveEntry(parent, name, true);
  if (!status.ok()) return status;

  // Decrement parent nlink: the removed subdirectory's ".." no longer points
  // back, so parent loses a hard link.
  if (parent_in) {
    parent_in->attr.st_nlink--;
  }

  parent_in->Touch(kMtime | kCtime);

  SWORDFS_LOG_DEBUG << "RmDir: parent=" << parent << " name='" << name
                    << "' ino=" << target_inode->ino;
  return Status::OK();
}

Status MemMetaImpl::Rename(InodeID old_parent,
                            std::string_view old_name, InodeID new_parent,
                            std::string_view new_name) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::string old_key(old_name);
  std::string new_key(new_name);

  // "." and ".." cannot be renamed
  if (old_key == "." || old_key == ".." || new_key == "." ||
      new_key == "..") {
    return Status::Busy("cannot rename . or ..");
  }

  // Both parents must be directories
  SwordFsInode* op = nullptr;
  Status status = store_.LookupInode(old_parent, &op);
  if (!status.ok() || !op || !op->IsDir()) {
    SWORDFS_LOG_ERROR << "Rename: old parent " << old_parent
                      << " is not a directory";
    return Status::NotDirectory("old parent is not a directory");
  }
  SwordFsInode* np = nullptr;
  status = store_.LookupInode(new_parent, &np);
  if (!status.ok() || !np || !np->IsDir()) {
    SWORDFS_LOG_ERROR << "Rename: new parent " << new_parent
                      << " is not a directory";
    return Status::NotDirectory("new parent is not a directory");
  }

  // Check write+execute permission on both parents
  if (op) {
    int r = Access(&op->attr, W_OK | X_OK);
    if (r != 0) return Status::Permission("access denied on old parent");
  }
  if (np) {
    int r = Access(&np->attr, W_OK | X_OK);
    if (r != 0) return Status::Permission("access denied on new parent");
  }

  SwordFsInode* moved_in = nullptr;
  status = store_.LookupEntry(old_parent, old_name, &moved_in);
  if (!status.ok()) {
    return Status::NotFound("source entry not found");
  }

  InodeID ino = moved_in->ino;
  bool is_dir = S_ISDIR(moved_in->attr.st_mode);

  // Cannot move a directory into its own subtree
  if (is_dir && store_.IsDescendantOf(ino, new_parent)) {
    return Status::InvalidArgument("cannot move directory into itself");
  }

  // Handle overwrite of an existing target
  SwordFsInode* existing_in = nullptr;
  if (store_.LookupEntry(new_parent, new_name, &existing_in).ok()) {
    if (!existing_in) {
      // Orphan entry — clean up and continue
      store_.RemoveEntry(new_parent, new_name);
    } else {
      bool existing_is_dir = S_ISDIR(existing_in->attr.st_mode);

      // Cannot replace a directory with a file or vice versa
      if (existing_is_dir != is_dir) {
        return Status::InvalidArgument(
            "cannot replace directory with non-directory");
      }

      status = store_.RemoveEntry(new_parent, new_name, true);
      if (!status.ok()) return status;

      if (existing_is_dir) {
        // Replacing an empty directory: decrement new_parent nlink
        if (np) np->attr.st_nlink--;
      }
    }
  }

  status = store_.MoveEntry(old_parent, old_name, new_parent, new_name);
  if (!status.ok()) return status;

  // Cross-directory move of a directory: adjust parent nlinks
  if (is_dir && old_parent != new_parent) {
    if (op) op->attr.st_nlink--;
    if (np) np->attr.st_nlink++;
  }

  // ctime of the moved inode is updated on rename
  moved_in->Touch(kCtime);
  if (op) op->Touch(kMtime | kCtime);
  if (np) np->Touch(kMtime | kCtime);

  SWORDFS_LOG_DEBUG << "Rename: " << old_parent << "/'" << old_name
                    << "' -> " << new_parent << "/'" << new_name << "'";
  return Status::OK();
}

Status MemMetaImpl::SetAttr(InodeID ino,
                             const struct stat* attr, int to_set,
                             struct stat* out_attr) {
  std::lock_guard<std::mutex> lock(mutex_);

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

  *out_attr = st;
  return Status::OK();
}

Status MemMetaImpl::StatFs(struct statvfs* stbuf) {
  std::lock_guard<std::mutex> lock(mutex_);
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
  std::lock_guard<std::mutex> lock(mutex_);

  SwordFsInode* inode = nullptr;
  store_.LookupInode(ino, &inode);
  if (!inode) {
    return Status::NotFound("inode not found");
  }

  int r = Access(&inode->attr, mask);
  if (r != 0) return Status::Permission("access denied");
  return Status::OK();
}

Status MemMetaImpl::Open(InodeID ino,
                          uint64_t* fh) {
  std::lock_guard<std::mutex> lock(mutex_);

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
  int r = Access(&inode->attr, R_OK);
  if (r != 0) return Status::Permission("access denied");

  uint64_t handle = AllocFh();
  file_handles_[handle] = ino;
  // Update atime on the file
  inode->attr.st_atime = ::time(nullptr);

  *fh = handle;
  return Status::OK();
}

Status MemMetaImpl::Release(uint64_t fh) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = file_handles_.find(fh);
  if (it == file_handles_.end()) {
    SWORDFS_LOG_ERROR << "Release: unknown file handle " << fh;
    return Status::InvalidArgument("unknown file handle");
  }

  file_handles_.erase(it);
  return Status::OK();
}

Status MemMetaImpl::OpenDir(InodeID ino,
                             uint64_t* fh) {
  std::lock_guard<std::mutex> lock(mutex_);

  SwordFsInode* dir_inode = nullptr;
  if (!store_.LookupInode(ino, &dir_inode).ok() || !dir_inode ||
      !dir_inode->IsDir()) {
    SWORDFS_LOG_ERROR << "OpenDir: ino " << ino << " is not a directory";
    return Status::NotDirectory("not a directory");
  }

  uint64_t handle = AllocFh();
  dir_handles_[handle] = ino;
  // Update atime on the directory
  dir_inode->Touch(kAtime);

  *fh = handle;
  return Status::OK();
}

Status MemMetaImpl::ReleaseDir(uint64_t fh) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = dir_handles_.find(fh);
  if (it == dir_handles_.end()) {
    SWORDFS_LOG_ERROR << "ReleaseDir: unknown dir handle " << fh;
    return Status::InvalidArgument("unknown dir handle");
  }

  dir_handles_.erase(it);
  return Status::OK();
}

Status MemMetaImpl::Forget(InodeID ino,
                            uint64_t nlookup) {
  std::lock_guard<std::mutex> lock(mutex_);

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

}  // namespace swordfs::metadata
