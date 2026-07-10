// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/MemMetaStore.hpp"

#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

#include "utils/Logging.hpp"

namespace swordfs::metadata {

namespace {

time_t NowSec() {
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch())
      .count();
}

struct stat MakeStat(uint64_t ino, mode_t mode, time_t now) {
  struct stat st;
  std::memset(&st, 0, sizeof(st));
  st.st_ino = ino;
  st.st_mode = mode;
  st.st_nlink = S_ISDIR(mode) ? 2 : 1;
  st.st_uid = ::getuid();
  st.st_gid = ::getgid();
  st.st_blksize = 4096;
  st.st_atime = now;
  st.st_mtime = now;
  st.st_ctime = now;
  return st;
}

// Convert st_mode to dirent type (DT_DIR, DT_REG, etc.)
uint32_t ModeToDt(mode_t mode) {
  if (S_ISDIR(mode)) return DT_DIR;
  if (S_ISREG(mode)) return DT_REG;
  if (S_ISLNK(mode)) return DT_LNK;
  if (S_ISBLK(mode)) return DT_BLK;
  if (S_ISCHR(mode)) return DT_CHR;
  if (S_ISFIFO(mode)) return DT_FIFO;
  if (S_ISSOCK(mode)) return DT_SOCK;
  return DT_UNKNOWN;
}

}  // namespace

// Construction

MemMetaStore::MemMetaStore() {
  time_t now = NowSec();
  struct stat root_st = MakeStat(FUSE_ROOT_ID, S_IFDIR | 0755, now);
  root_st.st_nlink = 2;  // . and root itself
  inodes_[FUSE_ROOT_ID] = {root_st, 0};
  dirs_[FUSE_ROOT_ID] = {};
}

// Helpers (called under lock)

uint64_t MemMetaStore::AllocIno() { return next_ino_++; }
uint64_t MemMetaStore::AllocFh() { return next_fh_++; }

bool MemMetaStore::IsDir(uint64_t ino) const {
  auto it = inodes_.find(ino);
  return it != inodes_.end() && S_ISDIR(it->second.attr.st_mode);
}

void MemMetaStore::TouchParent(uint64_t parent_ino) {
  auto it = inodes_.find(parent_ino);
  if (it == inodes_.end()) return;
  time_t now = NowSec();
  it->second.attr.st_mtime = now;
  it->second.attr.st_ctime = now;
}

void MemMetaStore::TouchCtime(uint64_t ino) {
  auto it = inodes_.find(ino);
  if (it == inodes_.end()) return;
  it->second.attr.st_ctime = NowSec();
}

void MemMetaStore::TouchAtime(uint64_t ino) {
  auto it = inodes_.find(ino);
  if (it == inodes_.end()) return;
  it->second.attr.st_atime = NowSec();
}

void MemMetaStore::KillSUID(struct stat* st) {
  if (st->st_mode & S_ISUID) st->st_mode &= ~S_ISUID;
  if (st->st_mode & S_ISGID) st->st_mode &= ~S_ISGID;
}

int MemMetaStore::Access(const struct stat* st, int mask) const {
  // Root always has full access
  if (::getuid() == 0) return 0;

  uid_t uid = ::getuid();
  gid_t gid = ::getgid();

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

  if ((mask & R_OK) && !(access_bits & 4)) return -EACCES;
  if ((mask & W_OK) && !(access_bits & 2)) return -EACCES;
  if ((mask & X_OK) && !(access_bits & 1)) return -EACCES;
  return 0;
}

// MetaStore interface

Status MemMetaStore::Lookup(uint64_t parent, std::string_view name,
                            uint64_t* child_ino, struct stat* attr) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Parent must be a directory
  if (!IsDir(parent)) {
    SWORDFS_LOG_ERROR << "Lookup: parent " << parent << " is not a directory";
    return Status::NotDirectory("parent is not a directory");
  }

  auto dir_it = dirs_.find(parent);
  if (dir_it == dirs_.end()) {
    SWORDFS_LOG_ERROR << "Lookup: parent " << parent << " has no dir map";
    return Status::NotFound("parent dir map not found");
  }

  std::string key(name);
  auto it = dir_it->second.find(key);
  if (it == dir_it->second.end()) {
    return Status::NotFound("entry not found");  // normal negative lookup, not an error
  }

  uint64_t ino = it->second;

  auto in_it = inodes_.find(ino);
  if (in_it == inodes_.end()) {
    SWORDFS_LOG_ERROR << "Lookup: inode " << ino << " (name '" << name
                      << "') missing";
    return Status::NotFound("inode not found");
  }

  // Increment lookup count so forget() can track when the kernel is done
  // referencing this inode.
  in_it->second.nlookup++;

  if (child_ino) *child_ino = ino;
  if (attr) *attr = in_it->second.attr;

  SWORDFS_LOG_DEBUG << "Lookup: parent=" << parent << " name='" << name
                    << "' -> ino=" << ino;
  return Status::OK();
}

Status MemMetaStore::GetAttr(uint64_t ino, struct stat* attr) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = inodes_.find(ino);
  if (it == inodes_.end()) {
    SWORDFS_LOG_ERROR << "GetAttr: ino " << ino << " not found";
    return Status::NotFound("inode not found");
  }

  *attr = it->second.attr;
  // POSIX: reading attributes updates the access time.
  it->second.attr.st_atime = NowSec();
  return Status::OK();
}

Status MemMetaStore::ReadDir(uint64_t ino,
                             std::vector<DirEntry>* entries) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!IsDir(ino)) {
    SWORDFS_LOG_ERROR << "ReadDir: ino " << ino << " is not a directory";
    return Status::NotDirectory("not a directory");
  }

  auto dir_it = dirs_.find(ino);
  if (dir_it == dirs_.end()) {
    SWORDFS_LOG_ERROR << "ReadDir: ino " << ino << " dir map not found";
    return Status::NotFound("dir map not found");
  }

  for (const auto& [name, child_ino] : dir_it->second) {
    auto in_it = inodes_.find(child_ino);
    if (in_it == inodes_.end()) continue;
    entries->push_back(
        {name, child_ino, ModeToDt(in_it->second.attr.st_mode)});
  }

  // Reading directory contents updates atime on the directory.
  TouchAtime(ino);
  return Status::OK();
}

Status MemMetaStore::Create(uint64_t parent, std::string_view name,
                            mode_t mode, uint64_t* child_ino,
                            struct stat* attr) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!IsDir(parent)) {
    SWORDFS_LOG_ERROR << "Create: parent " << parent << " is not a directory";
    return Status::NotDirectory("parent is not a directory");
  }

  auto dir_it = dirs_.find(parent);
  if (dir_it == dirs_.end()) {
    SWORDFS_LOG_ERROR << "Create: parent " << parent << " has no dir map";
    return Status::NotFound("parent dir map not found");
  }

  // Check permissions: need write+execute on the parent directory
  auto parent_in = inodes_.find(parent);
  if (parent_in != inodes_.end()) {
    int r = Access(&parent_in->second.attr, W_OK | X_OK);
    if (r != 0) return Status::Permission("access denied on parent");
  }

  std::string key(name);
  if (dir_it->second.count(key)) {
    return Status::AlreadyExists("entry already exists");
  }

  time_t now = NowSec();
  uint64_t ino = AllocIno();

  // Regular file: strip type bits, force S_IFREG, respect umask
  mode_t file_mode = (S_IFREG | (mode & 0777));
  struct stat st = MakeStat(ino, file_mode, now);
  st.st_nlink = 1;
  st.st_uid = ::getuid();
  st.st_gid = parent_in != inodes_.end() ? parent_in->second.attr.st_gid : 0;

  // Increment nlookup: fuse_reply_create counts as a lookup for the kernel.
  inodes_[ino] = {st, 1};
  dir_it->second[key] = ino;

  // Parent directory mtime/ctime must be updated after a child is created.
  TouchParent(parent);

  if (child_ino) *child_ino = ino;
  if (attr) *attr = st;

  SWORDFS_LOG_DEBUG << "Create: parent=" << parent << " name='" << name
                    << "' -> ino=" << ino;
  return Status::OK();
}

Status MemMetaStore::MkDir(uint64_t parent, std::string_view name,
                           mode_t mode, uint64_t* child_ino,
                           struct stat* attr) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!IsDir(parent)) {
    SWORDFS_LOG_ERROR << "MkDir: parent " << parent << " is not a directory";
    return Status::NotDirectory("parent is not a directory");
  }

  auto dir_it = dirs_.find(parent);
  if (dir_it == dirs_.end()) {
    SWORDFS_LOG_ERROR << "MkDir: parent " << parent << " has no dir map";
    return Status::NotFound("parent dir map not found");
  }

  auto parent_in = inodes_.find(parent);
  if (parent_in != inodes_.end()) {
    int r = Access(&parent_in->second.attr, W_OK | X_OK);
    if (r != 0) return Status::Permission("access denied on parent");
  }

  std::string key(name);
  if (dir_it->second.count(key)) {
    return Status::AlreadyExists("entry already exists");
  }

  time_t now = NowSec();
  uint64_t ino = AllocIno();

  mode_t dir_mode = (S_IFDIR | (mode & 0777));
  struct stat st = MakeStat(ino, dir_mode, now);
  st.st_uid = ::getuid();
  st.st_gid = parent_in != inodes_.end() ? parent_in->second.attr.st_gid : 0;

  // nlookup: fuse_reply_entry in mkdir response counts as a lookup.
  inodes_[ino] = {st, 1};
  dirs_[ino] = {};

  dir_it->second[key] = ino;

  // Increment parent nlink: the new subdirectory's ".." points back to the
  // parent, creating an additional hard link.
  if (parent_in != inodes_.end()) {
    parent_in->second.attr.st_nlink++;
  }

  TouchParent(parent);

  if (child_ino) *child_ino = ino;
  if (attr) *attr = st;

  SWORDFS_LOG_DEBUG << "MkDir: parent=" << parent << " name='" << name
                    << "' -> ino=" << ino;
  return Status::OK();
}

Status MemMetaStore::Unlink(uint64_t parent, std::string_view name) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!IsDir(parent)) {
    SWORDFS_LOG_ERROR << "Unlink: parent " << parent << " is not a directory";
    return Status::NotDirectory("parent is not a directory");
  }

  auto dir_it = dirs_.find(parent);
  if (dir_it == dirs_.end()) {
    SWORDFS_LOG_ERROR << "Unlink: parent " << parent << " has no dir map";
    return Status::NotFound("parent dir map not found");
  }

  // Permission check on parent directory
  auto parent_in = inodes_.find(parent);
  if (parent_in != inodes_.end()) {
    int r = Access(&parent_in->second.attr, W_OK | X_OK);
    if (r != 0) return Status::Permission("access denied on parent");

    // Sticky bit on directory: only the owner, directory owner, or root can
    // unlink entries.
    if (parent_in->second.attr.st_mode & S_ISVTX) {
      uid_t caller = ::getuid();
      if (caller != 0 && caller != parent_in->second.attr.st_uid) {
        auto it = dir_it->second.find(std::string(name));
        if (it != dir_it->second.end()) {
          auto target = inodes_.find(it->second);
          if (target != inodes_.end() &&
              caller != target->second.attr.st_uid) {
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

  auto it = dir_it->second.find(key);
  if (it == dir_it->second.end()) {
    return Status::NotFound("entry not found");
  }

  uint64_t ino = it->second;
  auto in_it = inodes_.find(ino);
  if (in_it != inodes_.end() && S_ISDIR(in_it->second.attr.st_mode)) {
    return Status::InvalidArgument("cannot unlink directory");
  }

  dir_it->second.erase(it);
  inodes_.erase(ino);

  TouchParent(parent);

  SWORDFS_LOG_DEBUG << "Unlink: parent=" << parent << " name='" << name
                    << "' ino=" << ino;
  return Status::OK();
}

Status MemMetaStore::RmDir(uint64_t parent, std::string_view name) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!IsDir(parent)) {
    SWORDFS_LOG_ERROR << "RmDir: parent " << parent << " is not a directory";
    return Status::NotDirectory("parent is not a directory");
  }

  auto dir_it = dirs_.find(parent);
  if (dir_it == dirs_.end()) {
    SWORDFS_LOG_ERROR << "RmDir: parent " << parent << " has no dir map";
    return Status::NotFound("parent dir map not found");
  }

  auto parent_in = inodes_.find(parent);
  if (parent_in != inodes_.end()) {
    int r = Access(&parent_in->second.attr, W_OK | X_OK);
    if (r != 0) return Status::Permission("access denied on parent");
  }

  std::string key(name);

  // Cannot remove "." or ".."
  if (key == "." || key == "..")
    return Status::InvalidArgument("cannot remove . or ..");

  // Cannot remove the root directory by name
  if (parent == FUSE_ROOT_ID && key == ".")
    return Status::Busy("root directory is busy");

  auto it = dir_it->second.find(key);
  if (it == dir_it->second.end()) {
    return Status::NotFound("entry not found");
  }

  uint64_t ino = it->second;

  auto in_it = inodes_.find(ino);
  if (in_it == inodes_.end() || !S_ISDIR(in_it->second.attr.st_mode)) {
    return Status::NotDirectory("not a directory");
  }

  // Directory must be empty (no child entries)
  auto child_dir = dirs_.find(ino);
  if (child_dir != dirs_.end() && !child_dir->second.empty()) {
    return Status::Busy("directory not empty");
  }

  dir_it->second.erase(it);
  dirs_.erase(ino);
  inodes_.erase(ino);

  // Decrement parent nlink: the removed subdirectory's ".." no longer points
  // back, so parent loses a hard link.
  if (parent_in != inodes_.end()) {
    parent_in->second.attr.st_nlink--;
  }

  TouchParent(parent);

  SWORDFS_LOG_DEBUG << "RmDir: parent=" << parent << " name='" << name
                    << "' ino=" << ino;
  return Status::OK();
}

namespace {

// Walk up the parent chain to determine whether `child` is a descendant of
// `ancestor`. Requires that dirs_ contains a complete parent→children map.
bool IsDescendantOf(
    uint64_t ancestor, uint64_t child,
    const std::unordered_map<uint64_t,
                             std::unordered_map<std::string, uint64_t>>& dirs) {
  if (ancestor == child) return true;
  // Walk all children of ancestor; if any child is a directory, recurse.
  auto it = dirs.find(ancestor);
  if (it == dirs.end()) return false;
  for (const auto& [_, sub_ino] : it->second) {
    auto sub = dirs.find(sub_ino);
    if (sub == dirs.end()) continue;  // files, not directories
    if (IsDescendantOf(sub_ino, child, dirs)) return true;
  }
  return false;
}

}  // namespace

Status MemMetaStore::Rename(uint64_t old_parent, std::string_view old_name,
                            uint64_t new_parent,
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
  if (!IsDir(old_parent)) {
    SWORDFS_LOG_ERROR << "Rename: old parent " << old_parent
                      << " is not a directory";
    return Status::NotDirectory("old parent is not a directory");
  }
  if (!IsDir(new_parent)) {
    SWORDFS_LOG_ERROR << "Rename: new parent " << new_parent
                      << " is not a directory";
    return Status::NotDirectory("new parent is not a directory");
  }

  auto old_dir = dirs_.find(old_parent);
  if (old_dir == dirs_.end()) {
    SWORDFS_LOG_ERROR << "Rename: old parent " << old_parent << " no dir map";
    return Status::NotFound("old parent dir map not found");
  }

  // Check write+execute permission on both parents
  auto op = inodes_.find(old_parent);
  if (op != inodes_.end()) {
    int r = Access(&op->second.attr, W_OK | X_OK);
    if (r != 0) return Status::Permission("access denied on old parent");
  }
  auto np = inodes_.find(new_parent);
  if (np != inodes_.end()) {
    int r = Access(&np->second.attr, W_OK | X_OK);
    if (r != 0) return Status::Permission("access denied on new parent");
  }

  auto old_entry = old_dir->second.find(old_key);
  if (old_entry == old_dir->second.end()) {
    return Status::NotFound("source entry not found");
  }

  auto new_dir = dirs_.find(new_parent);
  if (new_dir == dirs_.end()) {
    SWORDFS_LOG_ERROR << "Rename: new parent " << new_parent << " no dir map";
    return Status::NotFound("new parent dir map not found");
  }

  uint64_t ino = old_entry->second;
  auto moved_in = inodes_.find(ino);
  if (moved_in == inodes_.end()) {
    SWORDFS_LOG_ERROR << "Rename: inode " << ino << " for '" << old_name
                      << "' not found";
    return Status::NotFound("source inode not found");
  }

  bool is_dir = S_ISDIR(moved_in->second.attr.st_mode);

  // Cannot move a directory into its own subtree
  if (is_dir && IsDescendantOf(ino, new_parent, dirs_)) {
    return Status::InvalidArgument("cannot move directory into itself");
  }

  // Handle overwrite of an existing target
  auto existing = new_dir->second.find(new_key);
  if (existing != new_dir->second.end()) {
    auto existing_in = inodes_.find(existing->second);
    if (existing_in == inodes_.end()) {
      // Orphan entry — clean up and continue
      new_dir->second.erase(existing);
    } else {
      bool existing_is_dir = S_ISDIR(existing_in->second.attr.st_mode);

      // Cannot replace a directory with a file or vice versa
      if (existing_is_dir != is_dir) {
        return Status::InvalidArgument(
            "cannot replace directory with non-directory");
      }

      if (existing_is_dir) {
        // Target directory must be empty
        auto ed = dirs_.find(existing->second);
        if (ed != dirs_.end() && !ed->second.empty()) {
          return Status::Busy("target directory not empty");
        }
        // Replacing an empty directory: decrement new_parent nlink
        if (np != inodes_.end()) np->second.attr.st_nlink--;
      }

      new_dir->second.erase(existing);
      dirs_.erase(existing->second);
      inodes_.erase(existing->second);
    }
  }

  old_dir->second.erase(old_entry);
  new_dir->second[new_key] = ino;

  // Cross-directory move of a directory: adjust parent nlinks
  if (is_dir && old_parent != new_parent) {
    if (op != inodes_.end()) op->second.attr.st_nlink--;
    if (np != inodes_.end()) np->second.attr.st_nlink++;
  }

  // ctime of the moved inode is updated on rename
  TouchCtime(ino);
  TouchParent(old_parent);
  TouchParent(new_parent);

  SWORDFS_LOG_DEBUG << "Rename: " << old_parent << "/'" << old_name
                    << "' -> " << new_parent << "/'" << new_name << "'";
  return Status::OK();
}

Status MemMetaStore::SetAttr(uint64_t ino, const struct stat* attr,
                             int to_set, struct stat* out_attr) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = inodes_.find(ino);
  if (it == inodes_.end()) {
    SWORDFS_LOG_ERROR << "SetAttr: ino " << ino << " not found";
    return Status::NotFound("inode not found");
  }

  auto& st = it->second.attr;
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
    st.st_atime = NowSec();
    st.st_atim.tv_nsec = 0;
  }
  if (to_set & FUSE_SET_ATTR_MTIME_NOW) {
    st.st_mtime = NowSec();
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
    st.st_ctime = NowSec();
  }

  *out_attr = st;
  return Status::OK();
}

Status MemMetaStore::StatFs(struct statvfs* stbuf) {
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
  stbuf->f_files = inodes_.size();
  stbuf->f_ffree = UINT64_MAX;  // unlimited for in-memory store
  return Status::OK();
}

Status MemMetaStore::Access(uint64_t ino, int mask) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = inodes_.find(ino);
  if (it == inodes_.end()) {
    return Status::NotFound("inode not found");
  }

  int r = Access(&it->second.attr, mask);
  if (r != 0) return Status::Permission("access denied");
  return Status::OK();
}

Status MemMetaStore::Open(uint64_t ino, uint64_t* fh) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = inodes_.find(ino);
  if (it == inodes_.end()) {
    SWORDFS_LOG_ERROR << "Open: ino " << ino << " not found";
    return Status::NotFound("inode not found");
  }

  // Only regular files can be opened (directories use OpenDir, symlinks are
  // resolved by the kernel).
  if (!S_ISREG(it->second.attr.st_mode)) {
    SWORDFS_LOG_ERROR << "Open: ino " << ino << " is not a regular file";
    return Status::NotDirectory("not a regular file");
  }

  // Check read or write permission based on flags
  // (The kernel already passes filtered fi->flags to the FUSE daemon, so
  // O_RDONLY/O_WRONLY/O_RDWR are already set appropriately.)
  int r = Access(&it->second.attr, R_OK);
  if (r != 0) return Status::Permission("access denied");

  uint64_t handle = AllocFh();
  file_handles_[handle] = ino;
  // Update atime on the file
  it->second.attr.st_atime = NowSec();

  *fh = handle;
  return Status::OK();
}

Status MemMetaStore::Release(uint64_t fh) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = file_handles_.find(fh);
  if (it == file_handles_.end()) {
    SWORDFS_LOG_ERROR << "Release: unknown file handle " << fh;
    return Status::InvalidArgument("unknown file handle");
  }

  file_handles_.erase(it);
  return Status::OK();
}

Status MemMetaStore::OpenDir(uint64_t ino, uint64_t* fh) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!IsDir(ino)) {
    SWORDFS_LOG_ERROR << "OpenDir: ino " << ino << " is not a directory";
    return Status::NotDirectory("not a directory");
  }

  uint64_t handle = AllocFh();
  dir_handles_[handle] = ino;
  // Update atime on the directory
  TouchAtime(ino);

  *fh = handle;
  return Status::OK();
}

Status MemMetaStore::ReleaseDir(uint64_t fh) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = dir_handles_.find(fh);
  if (it == dir_handles_.end()) {
    SWORDFS_LOG_ERROR << "ReleaseDir: unknown dir handle " << fh;
    return Status::InvalidArgument("unknown dir handle");
  }

  dir_handles_.erase(it);
  return Status::OK();
}

void MemMetaStore::Forget(uint64_t ino, uint64_t nlookup) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = inodes_.find(ino);
  if (it == inodes_.end()) return;

  if (nlookup >= it->second.nlookup) {
    it->second.nlookup = 0;
  } else {
    it->second.nlookup -= nlookup;
  }
}

}  // namespace swordfs::metadata
