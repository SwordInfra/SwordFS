// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "storage/StorageRegistry.hpp"

namespace swordfs::storage {

StorageRegistry &StorageRegistry::Instance() {
  static StorageRegistry registry;
  return registry;
}

void StorageRegistry::Register(std::string_view name) {
  schemes_.insert(std::string(name));
}

bool StorageRegistry::Available(std::string_view name) const {
  return schemes_.find(std::string(name)) != schemes_.end();
}

}  // namespace swordfs::storage
