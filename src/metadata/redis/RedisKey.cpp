// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisKey.hpp"

#include <folly/Format.h>

namespace swordfs::metadata::redis {

RedisKey::RedisKey(int db, std::string_view volume_name) {
  prefix_ = folly::sformat("{{{}:{}}}:", db, volume_name);
}

std::string RedisKey::Format() const {
  return prefix_ + "format";
}

std::string RedisKey::NextIno() const {
  return prefix_ + "next_ino";
}

std::string RedisKey::Inode(uint64_t ino) const {
  return folly::sformat("{}inode:{}", prefix_, ino);
}

std::string RedisKey::Directory(uint64_t parent_ino) const {
  return folly::sformat("{}dir:{}", prefix_, parent_ino);
}

std::string RedisKey::Chunk(uint64_t ino) const {
  return folly::sformat("{}chunk:{}", prefix_, ino);
}

std::string RedisKey::InodeCount() const {
  return prefix_ + "inode_count";
}

}  // namespace swordfs::metadata::redis
