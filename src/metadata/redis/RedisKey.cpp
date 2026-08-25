// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisKey.hpp"

#include <folly/Format.h>

namespace swordfs::metadata {

RedisKey::RedisKey(int db, std::string_view volume_name) {
  prefix_ = folly::sformat("swordfs:{{{}:{}}}:", db, EncodeName(volume_name));
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

std::string RedisKey::Dentry(uint64_t parent_ino, std::string_view name) const {
  return folly::sformat("{}dentry:{}:{}", prefix_, parent_ino, EncodeName(name));
}

std::string RedisKey::DentryPrefix(uint64_t parent_ino) const {
  return folly::sformat("{}dentry:{}:", prefix_, parent_ino);
}

std::string RedisKey::Chunk(uint64_t ino, uint32_t index) const {
  return folly::sformat("{}chunk:{}:{}", prefix_, ino, index);
}

std::string RedisKey::ChunkPrefix(uint64_t ino) const {
  return folly::sformat("{}chunk:{}:", prefix_, ino);
}

std::string RedisKey::EncodeName(std::string_view name) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(name.size() * 2);
  for (const unsigned char c : name) {
    encoded.push_back(kHex[c >> 4]);
    encoded.push_back(kHex[c & 0x0f]);
  }
  return encoded;
}

}  // namespace swordfs::metadata
