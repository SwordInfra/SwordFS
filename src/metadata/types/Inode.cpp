// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/Inode.hpp"

#include <ctime>

#include "metadata/types/Serialization.hpp"

namespace swordfs::metadata {

SwordFsInode::SwordFsInode(InodeID ino, struct stat attr, InodeID parent_ino, std::string symlink_target)
    : ino(ino), attr(attr), parent_ino(parent_ino), symlink_target(std::move(symlink_target)) {
}

void SwordFsInode::Touch(SetAttrField fields) {
  time_t now = ::time(nullptr);
  if (HasSetAttrField(fields, SetAttrField::kAtime)) {
    attr.st_atime = now;
  }
  if (HasSetAttrField(fields, SetAttrField::kMtime)) {
    attr.st_mtime = now;
  }
  if (HasSetAttrField(fields, SetAttrField::kCtime)) {
    attr.st_ctime = now;
  }
}

bool SwordFsInode::IsDir() const {
  return S_ISDIR(attr.st_mode);
}
bool SwordFsInode::IsRegular() const {
  return S_ISREG(attr.st_mode);
}
bool SwordFsInode::IsSymlink() const {
  return S_ISLNK(attr.st_mode);
}

bool SwordFsInode::CheckAccess(uid_t uid, gid_t gid, int mask) const {
  if (uid == 0) {
    return true;
  }
  unsigned int access_bits;
  if (uid == attr.st_uid) {
    access_bits = (attr.st_mode & S_IRWXU) >> 6;
  } else if (gid == attr.st_gid) {
    access_bits = (attr.st_mode & S_IRWXG) >> 3;
  } else {
    access_bits = attr.st_mode & S_IRWXO;
  }
  return (access_bits & static_cast<unsigned int>(mask)) == static_cast<unsigned int>(mask);
}

bool SwordFsInode::CheckStickyDelete(uid_t uid, const SwordFsInode &target) const {
  if (!(attr.st_mode & S_ISVTX)) {
    return true;
  }
  return uid == 0 || uid == attr.st_uid || uid == target.attr.st_uid;
}

utils::Status SwordFsInode::SerializeTo(std::string *out) const {
  using namespace types::serialization;
  if (out == nullptr || ino == 0) {
    return utils::Status::InvalidArgument("Invalid inode record");
  }
  Writer writer;
  writer.Header(RecordType::kInode);
  writer.U64(ino);
  writer.U64(static_cast<uint64_t>(attr.st_dev));
  writer.U64(static_cast<uint64_t>(attr.st_ino));
  writer.U32(static_cast<uint32_t>(attr.st_mode));
  writer.U64(static_cast<uint64_t>(attr.st_nlink));
  writer.U64(static_cast<uint64_t>(attr.st_uid));
  writer.U64(static_cast<uint64_t>(attr.st_gid));
  writer.U64(static_cast<uint64_t>(attr.st_rdev));
  writer.U64(static_cast<uint64_t>(attr.st_size));
  writer.U64(static_cast<uint64_t>(attr.st_blksize));
  writer.U64(static_cast<uint64_t>(attr.st_blocks));
  WriteTimespec(writer, attr.st_atim);
  WriteTimespec(writer, attr.st_mtim);
  WriteTimespec(writer, attr.st_ctim);
  writer.U64(parent_ino);
  writer.String(symlink_target);
  writer.Finish(out);
  return utils::Status::OK();
}

utils::Status SwordFsInode::ParseFrom(std::string_view data, SwordFsInode *out) {
  using namespace types::serialization;
  if (out == nullptr) {
    return utils::Status::InvalidArgument("Inode output is null");
  }
  Reader reader(data);
  SwordFsInode inode;
  uint64_t ino = 0;
  uint64_t field = 0;
  if (!reader.Header(RecordType::kInode) || !reader.U64(&ino) || ino == 0 || !reader.U64(&field)) {
    return Malformed("inode");
  }
  inode.ino = ino;
  inode.attr.st_dev = static_cast<dev_t>(field);
  if (!reader.U64(&field)) {
    return Malformed("inode");
  }
  inode.attr.st_ino = static_cast<ino_t>(field);
  uint32_t mode = 0;
  if (!reader.U32(&mode)) {
    return Malformed("inode");
  }
  inode.attr.st_mode = static_cast<mode_t>(mode);
  if (!reader.U64(&field)) {
    return Malformed("inode");
  }
  inode.attr.st_nlink = static_cast<nlink_t>(field);
  if (!reader.U64(&field)) {
    return Malformed("inode");
  }
  inode.attr.st_uid = static_cast<uid_t>(field);
  if (!reader.U64(&field)) {
    return Malformed("inode");
  }
  inode.attr.st_gid = static_cast<gid_t>(field);
  if (!reader.U64(&field)) {
    return Malformed("inode");
  }
  inode.attr.st_rdev = static_cast<dev_t>(field);
  if (!reader.U64(&field)) {
    return Malformed("inode");
  }
  inode.attr.st_size = static_cast<off_t>(field);
  if (!reader.U64(&field)) {
    return Malformed("inode");
  }
  inode.attr.st_blksize = static_cast<blksize_t>(field);
  if (!reader.U64(&field)) {
    return Malformed("inode");
  }
  inode.attr.st_blocks = static_cast<blkcnt_t>(field);
  if (!ReadTimespec(reader, &inode.attr.st_atim) || !ReadTimespec(reader, &inode.attr.st_mtim) ||
      !ReadTimespec(reader, &inode.attr.st_ctim) || !reader.U64(&inode.parent_ino) ||
      !reader.String(&inode.symlink_target) || !reader.Done()) {
    return Malformed("inode");
  }
  if (inode.attr.st_ino != inode.ino || inode.attr.st_nlink == 0) {
    return Malformed("inode");
  }
  *out = std::move(inode);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
