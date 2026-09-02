// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "metadata/IMetaEngine.hpp"

namespace swordfs::metadata {

class RedisMetaClient;

class RedisDirIterator final : public DirIterator {
 public:
  RedisDirIterator(std::shared_ptr<RedisMetaClient> client, std::string key, std::vector<SwordFsEntry> prefix_entries);
  ~RedisDirIterator() override;

  RedisDirIterator(const RedisDirIterator &) = delete;
  RedisDirIterator &operator=(const RedisDirIterator &) = delete;

  Status Seek(uint64_t cookie) override;
  Status Peek(SwordFsEntry *entry, uint64_t *next_cookie) override;
  void Advance() override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace swordfs::metadata
