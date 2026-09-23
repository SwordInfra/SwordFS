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

std::string RedisKey::NextChunkRevision() const {
  return prefix_ + "next_chunk_revision";
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

std::string RedisKey::PrivateChunkIndex(std::string_view strategy, std::string_view hash) const {
  return folly::sformat("{}private_chunk_index:{}:{}", prefix_, strategy, hash);
}

std::string RedisKey::InodeCount() const {
  return prefix_ + "inode_count";
}

std::string RedisKey::Orphans() const {
  return prefix_ + "orphans";
}

std::string RedisKey::Reclaims() const {
  return prefix_ + "reclaims";
}

std::string RedisKey::PendingDeletes() const {
  return prefix_ + "pending_deletes";
}

}  // namespace swordfs::metadata::redis
