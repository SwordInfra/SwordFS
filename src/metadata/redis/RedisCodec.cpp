// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisCodec.hpp"

#include <sys/stat.h>

#include <limits>

namespace swordfs::metadata {
namespace RedisCodec {
namespace {

constexpr std::string_view kMagic = "SWFSRED1";
constexpr size_t kMaxStringLength = 16 * 1024 * 1024;

class Writer {
public:
  void U32(uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      data_.push_back(static_cast<char>(value >> (i * 8)));
    }
  }

  void U64(uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      data_.push_back(static_cast<char>(value >> (i * 8)));
    }
  }

  void String(std::string_view value) {
    U64(value.size());
    data_.append(value);
  }

  std::string Finish() && {
    return std::move(data_);
  }

private:
  std::string data_;
};

class Reader {
public:
  explicit Reader(std::string_view data) : data_(data) {
  }

  bool U32(uint32_t *value) {
    if (!CanRead(4)) {
      return false;
    }
    uint32_t result = 0;
    for (int i = 0; i < 4; ++i) {
      result |= static_cast<uint32_t>(static_cast<unsigned char>(data_[offset_++])) << (i * 8);
    }
    *value = result;
    return true;
  }

  bool U64(uint64_t *value) {
    if (!CanRead(8)) {
      return false;
    }
    uint64_t result = 0;
    for (int i = 0; i < 8; ++i) {
      result |= static_cast<uint64_t>(static_cast<unsigned char>(data_[offset_++])) << (i * 8);
    }
    *value = result;
    return true;
  }

  bool String(std::string *value) {
    uint64_t length = 0;
    if (!U64(&length) || length > kMaxStringLength || length > Remaining()) {
      return false;
    }
    value->assign(data_.substr(offset_, static_cast<size_t>(length)));
    offset_ += static_cast<size_t>(length);
    return true;
  }

  bool Done() const {
    return offset_ == data_.size();
  }

private:
  size_t Remaining() const {
    return data_.size() - offset_;
  }
  bool CanRead(size_t size) const {
    return size <= Remaining();
  }

  std::string_view data_;
  size_t offset_ = 0;
};

utils::Status Malformed(std::string_view type) {
  return utils::Status::InvalidArgument("Malformed Redis " + std::string(type) + " record");
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

void WriteHeader(Writer &writer) {
  writer.String(kMagic);
  writer.U32(kSchemaVersion);
}

bool ReadHeader(Reader &reader) {
  std::string magic;
  uint32_t version = 0;
  return reader.String(&magic) && magic == kMagic && reader.U32(&version) && version == kSchemaVersion;
}

}  // namespace

utils::Status EncodeFormat(const RedisFormat &format, std::string *out) {
  if (out == nullptr || format.schema_version != kSchemaVersion) {
    return utils::Status::InvalidArgument("Invalid Redis format record");
  }
  Writer writer;
  WriteHeader(writer);
  *out = std::move(writer).Finish();
  return utils::Status::OK();
}

utils::Status DecodeFormat(std::string_view value, RedisFormat *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("Redis format output is null");
  }
  Reader reader(value);
  RedisFormat format;
  if (!ReadHeader(reader) || !reader.Done()) {
    return Malformed("format");
  }
  format.schema_version = kSchemaVersion;
  *out = std::move(format);
  return utils::Status::OK();
}

utils::Status EncodeInode(const SwordFsInode &inode, std::string *out) {
  if (out == nullptr || inode.ino == 0) {
    return utils::Status::InvalidArgument("Invalid Redis inode record");
  }
  Writer writer;
  WriteHeader(writer);
  writer.U64(inode.ino);
  writer.U64(static_cast<uint64_t>(inode.attr.st_dev));
  writer.U64(static_cast<uint64_t>(inode.attr.st_ino));
  writer.U32(static_cast<uint32_t>(inode.attr.st_mode));
  writer.U64(static_cast<uint64_t>(inode.attr.st_nlink));
  writer.U64(static_cast<uint64_t>(inode.attr.st_uid));
  writer.U64(static_cast<uint64_t>(inode.attr.st_gid));
  writer.U64(static_cast<uint64_t>(inode.attr.st_rdev));
  writer.U64(static_cast<uint64_t>(inode.attr.st_size));
  writer.U64(static_cast<uint64_t>(inode.attr.st_blksize));
  writer.U64(static_cast<uint64_t>(inode.attr.st_blocks));
  WriteTimespec(writer, inode.attr.st_atim);
  WriteTimespec(writer, inode.attr.st_mtim);
  WriteTimespec(writer, inode.attr.st_ctim);
  writer.U64(inode.parent_ino);
  writer.String(inode.symlink_target);
  *out = std::move(writer).Finish();
  return utils::Status::OK();
}

utils::Status DecodeInode(std::string_view value, SwordFsInode *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("Redis inode output is null");
  }
  Reader reader(value);
  SwordFsInode inode;
  uint64_t ino = 0;
  uint64_t field = 0;
  if (!ReadHeader(reader) || !reader.U64(&ino) || ino == 0 || !reader.U64(&field)) {
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

utils::Status EncodeEntry(const SwordFsEntry &entry, std::string *out) {
  if (out == nullptr || entry.name.empty() || entry.name.size() > kMaxStringLength || entry.ino == 0) {
    return utils::Status::InvalidArgument("Invalid Redis directory entry record");
  }
  Writer writer;
  WriteHeader(writer);
  writer.String(entry.name);
  writer.U32(entry.type);
  writer.U64(entry.ino);
  *out = std::move(writer).Finish();
  return utils::Status::OK();
}

utils::Status DecodeEntry(std::string_view value, SwordFsEntry *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("Redis directory entry output is null");
  }
  Reader reader(value);
  SwordFsEntry entry;
  if (!ReadHeader(reader) || !reader.String(&entry.name) || entry.name.empty() || !reader.U32(&entry.type) ||
      !reader.U64(&entry.ino) || entry.ino == 0 || !reader.Done()) {
    return Malformed("directory entry");
  }
  *out = std::move(entry);
  return utils::Status::OK();
}

utils::Status EncodeChunk(const ChunkMeta &chunk, std::string *out) {
  if (out == nullptr || chunk.key.size() > kMaxStringLength) {
    return utils::Status::InvalidArgument("Invalid Redis chunk record");
  }
  Writer writer;
  WriteHeader(writer);
  writer.U32(chunk.index);
  writer.U64(chunk.start_offset);
  writer.String(chunk.key);
  writer.U64(chunk.size);
  *out = std::move(writer).Finish();
  return utils::Status::OK();
}

utils::Status DecodeChunk(std::string_view value, ChunkMeta *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("Redis chunk output is null");
  }
  Reader reader(value);
  ChunkMeta chunk;
  uint64_t size = 0;
  if (!ReadHeader(reader) || !reader.U32(&chunk.index) || !reader.U64(&chunk.start_offset) ||
      !reader.String(&chunk.key) || !reader.U64(&size) || size > std::numeric_limits<size_t>::max() || !reader.Done()) {
    return Malformed("chunk");
  }
  chunk.size = static_cast<size_t>(size);
  *out = std::move(chunk);
  return utils::Status::OK();
}

}  // namespace RedisCodec
}  // namespace swordfs::metadata
