// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <string>
#include <string_view>

#include "metadata/Types.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata::redis {

struct RedisFormatHeader {
  std::string magic;
  uint32_t schema_version;
};

utils::Status EncodeFormat(const RedisFormatHeader &format, std::string *out);
utils::Status DecodeFormat(std::string_view value, RedisFormatHeader *out);

utils::Status EncodeInode(const SwordFsInode &inode, std::string *out);
utils::Status DecodeInode(std::string_view value, SwordFsInode *out);

utils::Status EncodeEntry(const SwordFsEntry &entry, std::string *out);
utils::Status DecodeEntry(std::string_view value, SwordFsEntry *out);

utils::Status EncodeChunk(const ChunkMeta &chunk, std::string *out);
utils::Status DecodeChunk(std::string_view value, ChunkMeta *out);

}  // namespace swordfs::metadata::redis
