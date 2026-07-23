// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "storage/StorageRegistry.hpp"

namespace swordfs::storage {

StorageRegistry& StorageRegistry::Instance() {
  static StorageRegistry instance;
  return instance;
}

void StorageRegistry::Register(std::string_view name, Factory factory) {
  factories_[std::string(name)] = std::move(factory);
}

bool StorageRegistry::Available(std::string_view name) const {
  return factories_.find(std::string(name)) != factories_.end();
}

std::unique_ptr<IDataEngine> StorageRegistry::Create(
    std::string_view name) const {
  auto it = factories_.find(std::string(name));
  if (it == factories_.end()) return nullptr;
  return it->second();
}

}  // namespace swordfs::storage
