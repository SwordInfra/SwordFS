// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace swordfs::metadata::redis {

// Redis key layout for one SwordFS metadata namespace. The Redis logical DB
// and volume name form the namespace; the braces make the whole namespace a
// Redis Cluster hash tag so one volume stays on one slot when Cluster support
// is added.
class RedisKey {
 public:
  RedisKey(int db, std::string_view volume_name);

  std::string Format() const;
  std::string NextIno() const;
  std::string NextChunkRevision() const;
  std::string Inode(uint64_t ino) const;
  std::string Directory(uint64_t parent_ino) const;
  std::string Chunk(uint64_t ino) const;
  std::string InodeCount() const;

  // Orphan candidates: hash of inode id -> marker. Written in the same
  // transaction that drops an inode's nlink to zero; removed again when the
  // inode is revived, reclaimed, or found unreclaimable.
  std::string Orphans() const;

  // Pending reclaims: hash of inode id -> serialized ReclaimWork. Written by
  // reclaim preparation (the inode's point of no return) and removed once
  // every frozen object has been deleted.
  std::string Reclaims() const;

 private:
  std::string prefix_;
};

}  // namespace swordfs::metadata::redis
