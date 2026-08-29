// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/Inode.hpp"

#include <sys/stat.h>

#include <cstring>
#include <ctime>
#include <utility>

#include "metadata/types/BufCodec.hpp"

namespace swordfs::metadata {

SwordFsAttr::SwordFsAttr(uint64_t ino, uint32_t mode) : ino(ino), mode(mode) {
  nlink = S_ISDIR(mode) ? 2 : 1;
  size = S_ISDIR(mode) ? 4096 : 0;
  uid = static_cast<uint64_t>(::getuid());
  gid = static_cast<uint64_t>(::getgid());
  blksize = 4096;
  atime = mtime = ctime = static_cast<int64_t>(::time(nullptr));
}

void SwordFsAttr::KillSUID() {
  mode &= ~(S_ISUID | S_ISGID);
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
  st->st_blocks = static_cast<blkcnt_t>(blocks);
  st->st_atime = static_cast<time_t>(atime);
  st->st_atim.tv_nsec = static_cast<long>(atime_nsec);
  st->st_mtime = static_cast<time_t>(mtime);
  st->st_mtim.tv_nsec = static_cast<long>(mtime_nsec);
  st->st_ctime = static_cast<time_t>(ctime);
  st->st_ctim.tv_nsec = static_cast<long>(ctime_nsec);
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

bool SwordFsInode::IsDir() const {
  return S_ISDIR(attr.mode);
}

bool SwordFsInode::IsRegular() const {
  return S_ISREG(attr.mode);
}

bool SwordFsInode::IsSymlink() const {
  return S_ISLNK(attr.mode);
}

bool SwordFsInode::CheckAccess(uint64_t uid, uint64_t gid, uint32_t mask) const {
  if (uid == 0) {
    return true;
  }
  uint32_t access_bits;
  if (static_cast<uint64_t>(uid) == attr.uid) {
    access_bits = (attr.mode & S_IRWXU) >> 6;
  } else if (static_cast<uint64_t>(gid) == attr.gid) {
    access_bits = (attr.mode & S_IRWXG) >> 3;
  } else {
    access_bits = attr.mode & S_IRWXO;
  }
  return (access_bits & mask) == mask;
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
  if (!dec || ino == 0 || !dec.Done()) {
    return utils::Status::Malformed("Malformed inode record");
  }
  if (attr.ino != ino || attr.nlink == 0) {
    return utils::Status::Malformed("Malformed inode record");
  }
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
