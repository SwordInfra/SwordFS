// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/MetaEngineRegistry.hpp"

#include <algorithm>

namespace swordfs::metadata {

MetaEngineRegistry &MetaEngineRegistry::Instance() {
  static MetaEngineRegistry registry;
  return registry;
}

void MetaEngineRegistry::Register(std::string_view name, Factory factory, LimitsProvider limits) {
  engines_.emplace(std::string(name), Entry{factory, limits});
}

bool MetaEngineRegistry::Available(std::string_view name) const {
  return engines_.contains(std::string(name));
}

utils::Status MetaEngineRegistry::Create(std::string_view name, std::string_view meta_url,
                                         std::unique_ptr<IMetaEngine> *out) const {
  const auto it = engines_.find(std::string(name));
  if (it == engines_.end()) {
    return utils::Status::NotSupported("unsupported metadata engine: " + std::string(name));
  }
  return it->second.factory(meta_url, out);
}

Limits MetaEngineRegistry::GetLimits(std::string_view name) const {
  const auto it = engines_.find(std::string(name));
  if (it == engines_.end()) {
    return Limits{};
  }
  return it->second.limits();
}

std::vector<std::string> MetaEngineRegistry::Names() const {
  std::vector<std::string> names;
  names.reserve(engines_.size());
  for (const auto &[name, _] : engines_) {
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace swordfs::metadata
