// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace swordfs::metadata {

// Redis key layout for one SwordFS metadata namespace. The Redis logical DB
// and volume name form the namespace; the braces make the whole namespace a
// Redis Cluster hash tag so one volume stays on one slot when Cluster support
// is added.
class RedisKey {
public:
  RedisKey(int db, std::string_view volume_name);

  const std::string &Prefix() const {
    return prefix_;
  }
  std::string Format() const;
  std::string NextIno() const;
  std::string Inode(uint64_t ino) const;
  std::string Dentry(uint64_t parent_ino, std::string_view name) const;
  std::string DentryPrefix(uint64_t parent_ino) const;
  std::string Chunk(uint64_t ino, uint32_t index) const;
  std::string ChunkPrefix(uint64_t ino) const;

private:
  static std::string EncodeName(std::string_view name);

  std::string prefix_;
};

}  // namespace swordfs::metadata
