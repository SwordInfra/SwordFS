// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/Inode.hpp"

#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <cstring>
#include <ctime>
#include <utility>

#include "metadata/types/BufCodec.hpp"

namespace swordfs::metadata {

namespace {

uint64_t ProjectedPosixBlocks(const SwordFsAttr &attr) {
  uint64_t posix_blocks = attr.blocks;
  if (S_ISREG(attr.mode) && posix_blocks == 0 && attr.size != 0) {
    // generic/615 requires an already-visible non-empty file to never expose
    // zero allocated blocks during overwrite/writeback. Do not derive blocks
    // from logical size here: doing so turns sparse holes into allocation and
    // breaks sparse-file capability detection. Accurate allocation accounting
    // remains owned by the persisted blocks field when available.
    posix_blocks = 1;
  }
  return posix_blocks;
}

}  // namespace

SwordFsAttr::SwordFsAttr(uint64_t ino, uint32_t mode)
    : SwordFsAttr(ino, mode, static_cast<uint64_t>(::getuid()), static_cast<uint64_t>(::getgid())) {
}

SwordFsAttr::SwordFsAttr(uint64_t ino, uint32_t mode, uint64_t uid, uint64_t gid)
    : ino(ino), mode(mode), uid(uid), gid(gid) {
  nlink = S_ISDIR(mode) ? 2 : 1;
  size = S_ISDIR(mode) ? 4096 : 0;
  blksize = 4096;
  const int64_t now = static_cast<int64_t>(::time(nullptr));
  atime = mtime = ctime = btime = now;
}

void SwordFsAttr::ClearSetidForKillPriv() {
  mode &= ~S_ISUID;
  if (mode & S_IXGRP) {
    mode &= ~S_ISGID;
  }
}

void SwordFsAttr::ToPosixStat(struct stat *st) const {
  if (st == nullptr) {
    return;
  }
  std::memset(st, 0, sizeof(*st));
  st->st_dev = static_cast<dev_t>(dev);
  st->st_ino = static_cast<ino_t>(ino);
  st->st_mode = static_cast<mode_t>(mode);
  st->st_nlink = static_cast<nlink_t>(nlink);
  st->st_uid = static_cast<uid_t>(uid);
  st->st_gid = static_cast<gid_t>(gid);
  st->st_rdev = static_cast<dev_t>(rdev);
  st->st_size = static_cast<off_t>(size);
  st->st_blksize = static_cast<blksize_t>(blksize);
  st->st_blocks = static_cast<blkcnt_t>(ProjectedPosixBlocks(*this));
  st->st_atime = static_cast<time_t>(atime);
  st->st_atim.tv_nsec = static_cast<long>(atime_nsec);
  st->st_mtime = static_cast<time_t>(mtime);
  st->st_mtim.tv_nsec = static_cast<long>(mtime_nsec);
  st->st_ctime = static_cast<time_t>(ctime);
  st->st_ctim.tv_nsec = static_cast<long>(ctime_nsec);
}

void SwordFsAttr::ToStatX(struct statx *stx) const {
  std::memset(stx, 0, sizeof(*stx));
  stx->stx_mask = STATX_BASIC_STATS | STATX_BTIME;
  stx->stx_blksize = static_cast<uint32_t>(blksize);
  stx->stx_nlink = static_cast<uint32_t>(nlink);
  stx->stx_uid = static_cast<uint32_t>(uid);
  stx->stx_gid = static_cast<uint32_t>(gid);
  stx->stx_mode = static_cast<uint16_t>(mode);
  stx->stx_ino = ino;
  stx->stx_size = size;
  stx->stx_blocks = ProjectedPosixBlocks(*this);
  stx->stx_atime.tv_sec = atime;
  stx->stx_atime.tv_nsec = static_cast<uint32_t>(atime_nsec);
  stx->stx_btime.tv_sec = btime;
  stx->stx_btime.tv_nsec = static_cast<uint32_t>(btime_nsec);
  stx->stx_ctime.tv_sec = ctime;
  stx->stx_ctime.tv_nsec = static_cast<uint32_t>(ctime_nsec);
  stx->stx_mtime.tv_sec = mtime;
  stx->stx_mtime.tv_nsec = static_cast<uint32_t>(mtime_nsec);
  stx->stx_rdev_major = major(static_cast<dev_t>(rdev));
  stx->stx_rdev_minor = minor(static_cast<dev_t>(rdev));
  stx->stx_dev_major = major(static_cast<dev_t>(dev));
  stx->stx_dev_minor = minor(static_cast<dev_t>(dev));
  stx->stx_attributes_mask = STATX_ATTR_IMMUTABLE | STATX_ATTR_APPEND;
  if (HasInodeFlag(inode_flags, InodeFlag::kImmutable)) {
    stx->stx_attributes |= STATX_ATTR_IMMUTABLE;
  }
  if (HasInodeFlag(inode_flags, InodeFlag::kAppendOnly)) {
    stx->stx_attributes |= STATX_ATTR_APPEND;
  }
}

SwordFsAttr SwordFsAttr::FromPosixStat(const struct stat &st) {
  SwordFsAttr attr;
  attr.dev = static_cast<uint64_t>(st.st_dev);
  attr.ino = static_cast<uint64_t>(st.st_ino);
  attr.mode = static_cast<uint32_t>(st.st_mode);
  attr.nlink = static_cast<uint64_t>(st.st_nlink);
  attr.uid = static_cast<uint64_t>(st.st_uid);
  attr.gid = static_cast<uint64_t>(st.st_gid);
  attr.rdev = static_cast<uint64_t>(st.st_rdev);
  attr.size = static_cast<uint64_t>(st.st_size);
  attr.blksize = static_cast<uint64_t>(st.st_blksize);
  attr.blocks = static_cast<uint64_t>(st.st_blocks);
  attr.atime = static_cast<int64_t>(st.st_atime);
  attr.atime_nsec = static_cast<int64_t>(st.st_atim.tv_nsec);
  attr.mtime = static_cast<int64_t>(st.st_mtime);
  attr.mtime_nsec = static_cast<int64_t>(st.st_mtim.tv_nsec);
  attr.ctime = static_cast<int64_t>(st.st_ctime);
  attr.ctime_nsec = static_cast<int64_t>(st.st_ctim.tv_nsec);
  return attr;
}

SwordFsInode::SwordFsInode(InodeID ino, SwordFsAttr attr, InodeID parent_ino, std::string symlink_target)
    : ino(ino), attr(attr), parent_ino(parent_ino), symlink_target(std::move(symlink_target)) {
}

void SwordFsInode::Touch(SetAttrField fields) {
  const int64_t now = static_cast<int64_t>(::time(nullptr));
  if (HasSetAttrField(fields, SetAttrField::kAtime)) {
    attr.atime = now;
    attr.atime_nsec = 0;
  }
  if (HasSetAttrField(fields, SetAttrField::kMtime)) {
    attr.mtime = now;
    attr.mtime_nsec = 0;
  }
  if (HasSetAttrField(fields, SetAttrField::kCtime)) {
    attr.ctime = now;
    attr.ctime_nsec = 0;
  }
}

utils::Status SwordFsInode::SetXAttr(std::string_view name, std::string_view value, XAttrSetMode mode) {
  if (name.empty()) {
    return utils::Status::InvalidArgument("xattr name is empty");
  }
  if (name.size() > kMaxXAttrNameLength) {
    return utils::Status::Range("xattr name exceeds maximum length");
  }
  if (value.size() > kMaxXAttrValueSize) {
    return utils::Status::Range("xattr value exceeds maximum size");
  }
  const std::string key(name);
  auto it = xattrs.find(key);
  switch (mode) {
    case XAttrSetMode::kUpsert:
      break;
    case XAttrSetMode::kCreateOnly:
      if (it != xattrs.end()) {
        return utils::Status::AlreadyExists("xattr already exists");
      }
      break;
    case XAttrSetMode::kReplaceOnly:
      if (it == xattrs.end()) {
        return utils::Status::NoData("xattr not found");
      }
      break;
    default:
      return utils::Status::InvalidArgument("invalid xattr set mode");
  }

  const bool inserts_name = it == xattrs.end();
  if (inserts_name) {
    size_t list_bytes = name.size() + 1;
    for (const auto &[existing_name, existing_value] : xattrs) {
      (void)existing_value;
      list_bytes += existing_name.size() + 1;
    }
    if (list_bytes > kMaxXAttrListSize) {
      return utils::Status::Range("xattr name list exceeds maximum size");
    }
  }

  if (inserts_name) {
    xattrs.emplace(key, std::string(value));
  } else {
    it->second.assign(value);
  }
  Touch(SetAttrField::kCtime);
  return utils::Status::OK();
}

utils::Status SwordFsInode::GetXAttr(std::string_view name, std::string *value) const {
  if (value == nullptr) {
    return utils::Status::InvalidArgument("xattr value output is null");
  }
  if (name.size() > kMaxXAttrNameLength) {
    return utils::Status::Range("xattr name exceeds maximum length");
  }
  auto it = xattrs.find(std::string(name));
  if (it == xattrs.end()) {
    return utils::Status::NoData("xattr not found");
  }
  *value = it->second;
  return utils::Status::OK();
}

utils::Status SwordFsInode::ListXAttrs(std::vector<std::string> *names) const {
  if (names == nullptr) {
    return utils::Status::InvalidArgument("xattr names output is null");
  }
  names->clear();
  names->reserve(xattrs.size());
  for (const auto &[name, value] : xattrs) {
    (void)value;
    names->push_back(name);
  }
  return utils::Status::OK();
}

utils::Status SwordFsInode::RemoveXAttr(std::string_view name) {
  if (name.size() > kMaxXAttrNameLength) {
    return utils::Status::Range("xattr name exceeds maximum length");
  }
  auto it = xattrs.find(std::string(name));
  if (it == xattrs.end()) {
    return utils::Status::NoData("xattr not found");
  }
  xattrs.erase(it);
  Touch(SetAttrField::kCtime);
  return utils::Status::OK();
}

bool SwordFsInode::IsDir() const {
  return S_ISDIR(attr.mode);
}

bool SwordFsInode::IsRegular() const {
  return S_ISREG(attr.mode);
}

bool SwordFsInode::IsSymlink() const {
  return S_ISLNK(attr.mode);
}

bool SwordFsInode::CheckStickyDelete(uint64_t uid, const SwordFsInode &target) const {
  if (!(attr.mode & S_ISVTX)) {
    return true;
  }
  return uid == 0 || uid == attr.uid || uid == target.attr.uid;
}

utils::Status SwordFsInode::SerializeTo(std::string *out) const {
  if (out == nullptr || ino == 0) {
    return utils::Status::InvalidArgument("Invalid inode record");
  }
  BufEncoder enc;
  enc.Header(RecordType::kInode);
  enc.U64(ino);
  enc.Attr(attr);
  enc.U64(parent_ino);
  enc.String(symlink_target);
  enc.U64(xattrs.size());
  for (const auto &[name, value] : xattrs) {
    enc.String(name);
    enc.String(value);
  }
  enc.Finish(out);
  return utils::Status::OK();
}

utils::Status SwordFsInode::ParseFrom(std::string_view data) {
  BufDecoder dec(data);
  dec.Header(RecordType::kInode);
  dec.U64(&ino);
  dec.Attr(&attr);
  dec.U64(&parent_ino);
  dec.String(&symlink_target);
  uint64_t xattr_count = 0;
  dec.U64(&xattr_count);
  xattrs.clear();
  for (uint64_t i = 0; i < xattr_count; ++i) {
    std::string name;
    std::string value;
    if (!dec.String(&name) || !dec.String(&value)) {
      return utils::Status::Malformed("Malformed inode record: xattr decode failure");
    }
    if (name.empty()) {
      return utils::Status::Malformed("Malformed inode record: empty xattr name");
    }
    if (!xattrs.emplace(std::move(name), std::move(value)).second) {
      return utils::Status::Malformed("Malformed inode record: duplicate xattr name");
    }
  }
  if (!dec || ino == 0) {
    return utils::Status::Malformed("Malformed inode record: decode failure");
  }
  if (!dec.Done()) {
    return utils::Status::Malformed("Malformed inode record: trailing data size=" + std::to_string(data.size()));
  }
  // nlink == 0 is a valid orphan-inode state: the inode remains in metadata
  // until the reclaim lifecycle freezes it (IMetaEngine::PrepareReclaim),
  // and a hard link revives it (nlink back above zero) until then.
  if (attr.ino != ino) {
    return utils::Status::Malformed("Malformed inode record: inode number mismatch");
  }
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
