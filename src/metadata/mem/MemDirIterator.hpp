// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "metadata/IMetaEngine.hpp"

namespace swordfs::metadata {

class MemDirIterator final : public DirIterator {
 public:
  explicit MemDirIterator(std::vector<SwordFsEntry> entries);
  ~MemDirIterator() override = default;

  MemDirIterator(const MemDirIterator &) = delete;
  MemDirIterator &operator=(const MemDirIterator &) = delete;

  Status Seek(uint64_t cookie) override;
  Status Peek(SwordFsEntry *entry, uint64_t *next_cookie) override;
  void Advance() override;

 private:
  std::vector<SwordFsEntry> entries_;
  uint64_t position_ = 0;
  std::optional<uint64_t> pending_next_;
};

}  // namespace swordfs::metadata
