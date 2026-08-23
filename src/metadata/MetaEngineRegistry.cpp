// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/MetaEngineRegistry.hpp"

#include <algorithm>

namespace swordfs::metadata {

MetaEngineRegistry& MetaEngineRegistry::Instance() {
  static MetaEngineRegistry registry;
  return registry;
}

void MetaEngineRegistry::Register(std::string_view name) {
  engines_.push_back(name);
}

bool MetaEngineRegistry::Available(std::string_view name) const {
  return std::any_of(engines_.begin(), engines_.end(),
                     [name](std::string_view engine) {
                       return engine == name;
                     });
}

}  // namespace swordfs::metadata
