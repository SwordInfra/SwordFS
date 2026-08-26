// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/Types.hpp"

#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <sys/stat.h>

#include <ctime>
#include <limits>
#include <memory>

namespace swordfs::metadata {
namespace {

constexpr size_t kInitialBufferSize = 1024;
constexpr size_t kMaxStringLength = 16 * 1024 * 1024;
constexpr std::string_view kMagic = "SWFSMETA";
constexpr uint32_t kSchemaVersion = 1;

enum class RecordType : uint32_t {
  kInode = 1,
  kEntry = 2,
  kChunk = 3,
};

class Writer {
 public:
  Writer()
      : buffer_(folly::IOBuf::create(kInitialBufferSize)),
        appender_(buffer_.get(), kInitialBufferSize) {}

  void U32(uint32_t value) { appender_.writeLE<uint32_t>(value); }
  void U64(uint64_t value) { appender_.writeLE<uint64_t>(value); }

  void String(std::string_view value) {
    U64(value.size());
    appender_.push(reinterpret_cast<const uint8_t *>(value.data()), value.size());
  }

  void Header(RecordType type) {
    String(kMagic);
    U32(kSchemaVersion);
    U32(static_cast<uint32_t>(type));
  }

  void Finish(std::string *out) { buffer_->appendTo(*out); }

 private:
  std::unique_ptr<folly::IOBuf> buffer_;
  folly::io::Appender appender_;
};

class Reader {
 public:
  explicit Reader(std::string_view data)
      : buffer_(folly::IOBuf::wrapBuffer(data.data(), data.size())),
        cursor_(buffer_.get()) {}

  bool U32(uint32_t *value) { return cursor_.tryReadLE(*value); }
  bool U64(uint64_t *value) { return cursor_.tryReadLE(*value); }

  bool String(std::string *value) {
    uint64_t length = 0;
    if (!U64(&length) || length > kMaxStringLength || !cursor_.canAdvance(length)) {
      return false;
    }
    *value = cursor_.readFixedString(length);
    return true;
  }

  bool Header(RecordType expected_type) {
    std::string magic;
    uint32_t version = 0;
    uint32_t type = 0;
    return String(&magic) && magic == kMagic && U32(&version) &&
           version == kSchemaVersion && U32(&type) &&
           type == static_cast<uint32_t>(expected_type);
  }

  bool Done() const { return cursor_.isAtEnd(); }

 private:
  std::unique_ptr<folly::IOBuf> buffer_;
  folly::io::Cursor cursor_;
};

utils::Status Malformed(std::string_view type) {
  return utils::Status::Malformed("Malformed metadata " + std::string(type) + " record");
}

void WriteTimespec(Writer &writer, const struct timespec &ts) {
  writer.U64(static_cast<uint64_t>(static_cast<int64_t>(ts.tv_sec)));
  writer.U64(static_cast<uint64_t>(static_cast<int64_t>(ts.tv_nsec)));
}

bool ReadTimespec(Reader &reader, struct timespec *ts) {
  uint64_t sec = 0;
  uint64_t nsec = 0;
  if (!reader.U64(&sec) || !reader.U64(&nsec)) {
    return false;
  }
  ts->tv_sec = static_cast<time_t>(static_cast<int64_t>(sec));
  ts->tv_nsec = static_cast<long>(static_cast<int64_t>(nsec));
  return ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000L;
}

}  // namespace

SwordFsInode::SwordFsInode(InodeID ino, struct stat attr,
                           InodeID parent_ino, std::string symlink_target)
    : ino(ino),
      attr(attr),
      parent_ino(parent_ino),
      symlink_target(std::move(symlink_target)) {}

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

bool SwordFsInode::IsDir() const { return S_ISDIR(attr.st_mode); }

bool SwordFsInode::IsRegular() const { return S_ISREG(attr.st_mode); }

bool SwordFsInode::IsSymlink() const { return S_ISLNK(attr.st_mode); }

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
    access_bits = (attr.st_mode & S_IRWXO);
  }
  return (access_bits & static_cast<unsigned int>(mask)) ==
         static_cast<unsigned int>(mask);
}

bool SwordFsInode::CheckStickyDelete(uid_t uid,
                                     const SwordFsInode &target) const {
  if (!(attr.st_mode & S_ISVTX)) {
    return true;
  }
  return uid == 0 || uid == attr.st_uid || uid == target.attr.st_uid;
}

utils::Status SwordFsInode::SerializeTo(std::string *out) const {
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

utils::Status SwordFsInode::ParseFrom(std::string_view data,
                                       SwordFsInode *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("Inode output is null");
  }

  Reader reader(data);
  SwordFsInode inode;
  uint64_t ino = 0;
  uint64_t field = 0;
  if (!reader.Header(RecordType::kInode) || !reader.U64(&ino) || ino == 0 ||
      !reader.U64(&field)) {
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
  if (!ReadTimespec(reader, &inode.attr.st_atim) ||
      !ReadTimespec(reader, &inode.attr.st_mtim) ||
      !ReadTimespec(reader, &inode.attr.st_ctim) ||
      !reader.U64(&inode.parent_ino) ||
      !reader.String(&inode.symlink_target) || !reader.Done()) {
    return Malformed("inode");
  }
  if (inode.attr.st_ino != inode.ino || inode.attr.st_nlink == 0) {
    return Malformed("inode");
  }
  *out = std::move(inode);
  return utils::Status::OK();
}

utils::Status SwordFsEntry::SerializeTo(std::string *out) const {
  if (out == nullptr || name.empty() || name.size() > kMaxStringLength || ino == 0) {
    return utils::Status::InvalidArgument("Invalid directory entry record");
  }
  Writer writer;
  writer.Header(RecordType::kEntry);
  writer.String(name);
  writer.U32(type);
  writer.U64(ino);
  writer.Finish(out);
  return utils::Status::OK();
}

utils::Status SwordFsEntry::ParseFrom(std::string_view data,
                                       SwordFsEntry *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("Directory entry output is null");
  }
  Reader reader(data);
  SwordFsEntry entry;
  if (!reader.Header(RecordType::kEntry) || !reader.String(&entry.name) ||
      entry.name.empty() || !reader.U32(&entry.type) || !reader.U64(&entry.ino) ||
      entry.ino == 0 || !reader.Done()) {
    return Malformed("directory entry");
  }
  *out = std::move(entry);
  return utils::Status::OK();
}

utils::Status ChunkMeta::SerializeTo(std::string *out) const {
  if (out == nullptr || key.size() > kMaxStringLength) {
    return utils::Status::InvalidArgument("Invalid chunk record");
  }
  Writer writer;
  writer.Header(RecordType::kChunk);
  writer.U32(index);
  writer.U64(start_offset);
  writer.String(key);
  writer.U64(size);
  writer.Finish(out);
  return utils::Status::OK();
}

utils::Status ChunkMeta::ParseFrom(std::string_view data, ChunkMeta *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("Chunk output is null");
  }
  Reader reader(data);
  ChunkMeta chunk;
  uint64_t size = 0;
  if (!reader.Header(RecordType::kChunk) || !reader.U32(&chunk.index) ||
      !reader.U64(&chunk.start_offset) || !reader.String(&chunk.key) ||
      !reader.U64(&size) || size > std::numeric_limits<size_t>::max() ||
      !reader.Done()) {
    return Malformed("chunk");
  }
  chunk.size = static_cast<size_t>(size);
  *out = std::move(chunk);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
