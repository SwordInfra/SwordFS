// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/MetaEngineRegistry.hpp"

namespace swordfs::metadata {

MetaEngineRegistry &MetaEngineRegistry::Instance() {
  static MetaEngineRegistry registry;
  return registry;
}

void MetaEngineRegistry::Register(std::string_view name, Factory factory) {
  engines_.emplace(std::string(name), Entry{factory});
}

bool MetaEngineRegistry::Available(std::string_view name) const {
  return engines_.contains(std::string(name));
}

utils::Status MetaEngineRegistry::Create(std::string_view name, std::string_view meta_url, std::string_view volume_name,
                                         std::unique_ptr<IMetaEngine> *out) const {
  const auto it = engines_.find(std::string(name));
  if (it == engines_.end()) {
    return utils::Status::NotSupported("unsupported metadata engine: " + std::string(name));
  }
  return it->second.factory(meta_url, volume_name, out);
}

}  // namespace swordfs::metadata
