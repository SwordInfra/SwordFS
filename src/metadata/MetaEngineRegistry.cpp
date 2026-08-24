// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/MetaEngineRegistry.hpp"

namespace swordfs::metadata {

MetaEngineRegistry &MetaEngineRegistry::Instance() {
  static MetaEngineRegistry registry;
  return registry;
}

void MetaEngineRegistry::Register(std::string_view name) {
  engines_.emplace(name);
}

bool MetaEngineRegistry::Available(std::string_view name) const {
  return engines_.contains(std::string(name));
}

}  // namespace swordfs::metadata
