// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/Handle.hpp"

#include <folly/container/F14Map.h>
#include <folly/logging/xlog.h>

namespace swordfs::vfs {

struct HandleMap : folly::F14FastMap<uint64_t, std::shared_ptr<Handle>> {};

// ────────────────────────────────────────────────────────────────
// HandleManager
// ────────────────────────────────────────────────────────────────

HandleManager::HandleManager() : handles_(std::make_unique<HandleMap>()) {
}

HandleManager &HandleManager::Instance() {
  static HandleManager instance;
  return instance;
}

uint64_t HandleManager::Register(std::shared_ptr<Handle> handle) {
  CHECK(handle != nullptr);
  std::unique_lock lock(mutex_);
  const auto fh = next_fh_++;
  handle->fh_ = fh;
  auto [it, inserted] = handles_->emplace(fh, std::move(handle));
  CHECK(inserted);
  return fh;
}

std::shared_ptr<Handle> HandleManager::FindHandle(uint64_t fh) {
  std::shared_lock lock(mutex_);
  auto it = handles_->find(fh);
  if (it == handles_->end()) {
    return nullptr;
  }
  return it->second;
}

void HandleManager::Unregister(uint64_t fh) {
  std::unique_lock lock(mutex_);
  handles_->erase(fh);
}

}  // namespace swordfs::vfs
